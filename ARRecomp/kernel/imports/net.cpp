// Networking. The online services the game used are gone. Normally the console behaves as if it has no network connection: startup succeeds, there is no
// network address and socket operations fail with WSAENETDOWN.
//
// With AR_ONLINE (an experiment, see docs/LESSONS.md) sockets are real ones and copies of the game find each other directly, without any service:
// - Each copy has a block of 16 UDP/TCP ports from 36000 up, the first free one, so several can run on one machine. A title port becomes base + (port & 15).
// - The title sees other copies through "secure addresses" (1.0.0.n), each standing for a machine's IP and port base, as it would on Xbox Live.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/net_session.h>
#include <random>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

constexpr int32_t X_SOCKET_ERROR = -1;
constexpr uint32_t X_WSAEINVAL = 10022;
constexpr uint32_t X_WSAENETDOWN = 10050;
constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;

static thread_local uint32_t t_lastSocketError;

namespace net
{
    bool Online()
    {
        static const bool online = AR_ENV("AR_ONLINE") != nullptr;
        return online;
    }

    static std::once_flag g_started;
    static uint16_t g_base;
    static uint32_t g_localIp; // Network byte order, as in memory.
    static SOCKET g_portHolder = INVALID_SOCKET;
    static std::array<uint8_t, 20> g_onlineId;

    static void Start()
    {
        std::call_once(g_started, []
            {
                WSADATA data;
                WSAStartup(MAKEWORD(2, 2), &data);
                // The first free block of ports.
                for (uint16_t base = BASE_PORT; base < BASE_PORT + PORT_BLOCK * MAX_INSTANCES; base += PORT_BLOCK)
                {
                    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    sockaddr_in address{};
                    address.sin_family = AF_INET;
                    address.sin_port = htons(uint16_t(base + DISCOVERY_OFFSET));
                    if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                    {
                        g_base = base;
                        g_portHolder = s;
                        break;
                    }
                    closesocket(s);
                }

                // This machine's address on its network: the one a connection out to the world would use (nothing is sent). AR_NET_IP overrides it.
                g_localIp = htonl(INADDR_LOOPBACK);
                if (const char* text = AR_ENV("AR_NET_IP"))
                    inet_pton(AF_INET, text, &g_localIp);
                else
                {
                    SOCKET probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    sockaddr_in world{};
                    world.sin_family = AF_INET;
                    world.sin_port = htons(53);
                    inet_pton(AF_INET, "8.8.8.8", &world.sin_addr);
                    if (::connect(probe, reinterpret_cast<sockaddr*>(&world), sizeof(world)) == 0)
                    {
                        sockaddr_in me{};
                        int size = sizeof(me);
                        if (getsockname(probe, reinterpret_cast<sockaddr*>(&me), &size) == 0)
                            g_localIp = me.sin_addr.s_addr;
                    }
                    closesocket(probe);
                }

                std::mt19937 random{ std::random_device{}() };
                for (uint8_t& byte : g_onlineId)
                    byte = uint8_t(random());
                uint8_t* ip = reinterpret_cast<uint8_t*>(&g_localIp);
                LOGF_INFO("online: {}.{}.{}.{} ports {} up", ip[0], ip[1], ip[2], ip[3], g_base);
                StartDiscovery(uint64_t(g_portHolder));
            });
    }

    uint16_t LocalBase()
    {
        Start();
        return g_base;
    }

    // Peers, by their index in the secure address.
    struct Peer
    {
        uint32_t ip;
        uint16_t base;
        uint8_t xnaddr[36];
        bool hasAddress;
    };
    static std::mutex g_peersMutex;
    static std::vector<Peer> g_peers;

    static uint32_t SecureAddress(size_t index)
    {
        // In memory: 1.0.0.n (or 1.0.m.n).
        return htonl(0x01000000u | uint32_t(index + 1));
    }

    static uint32_t PeerByReal(uint32_t ip, uint16_t base)
    {
        std::lock_guard lock(g_peersMutex);
        for (size_t i = 0; i < g_peers.size(); i++)
        {
            if (g_peers[i].ip == ip && g_peers[i].base == base)
                return SecureAddress(i);
        }
        g_peers.push_back({ ip, base, {}, false });
        return SecureAddress(g_peers.size() - 1);
    }

    static bool RealFromSecure(uint32_t secure, uint32_t& ip, uint16_t& base)
    {
        uint32_t value = ntohl(secure);
        if ((value & 0xFF000000) != 0x01000000)
            return false;
        std::lock_guard lock(g_peersMutex);
        size_t index = (value & 0xFFFFFF) - 1;
        if (index >= g_peers.size())
            return false;
        ip = g_peers[index].ip;
        base = g_peers[index].base;
        return true;
    }

    // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
    void FillLocalAddress(uint8_t* address)
    {
        Start();
        memset(address, 0, 0x24);
        memcpy(address + 0, &g_localIp, 4);
        memcpy(address + 4, &g_localIp, 4);
        uint16_t port = htons(g_base);
        memcpy(address + 8, &port, 2);
        // A machine id of sorts, different per copy.
        address[10] = 0x02;
        address[11] = 0x41;
        address[12] = 0x52;
        memcpy(address + 13, &port, 2);
        address[15] = uint8_t(g_localIp >> 24);
        memcpy(address + 16, g_onlineId.data(), 20);
    }

    // What the title's sockets are, by guest handle.
    struct GuestSocket
    {
        SOCKET host = INVALID_SOCKET;
        bool datagram = false;
    };
    static std::mutex g_socketsMutex;
    static std::unordered_map<uint32_t, GuestSocket> g_sockets;
    static uint32_t g_nextSocket = 0x100;
    static uint16_t g_titlePorts[PORT_BLOCK];

    static GuestSocket FindSocket(uint32_t handle)
    {
        std::lock_guard lock(g_socketsMutex);
        auto it = g_sockets.find(handle);
        return it != g_sockets.end() ? it->second : GuestSocket{};
    }

    static uint16_t ToHostPort(uint16_t titlePort, uint16_t base)
    {
        return uint16_t(base + (titlePort & (PORT_BLOCK - 1)));
    }

    // A guest's sockaddr_in: family (big endian), port and address as in memory on any machine.
    static sockaddr_in ReadAddress(uint32_t guest)
    {
        const uint8_t* data = g_memory.Translate<const uint8_t>(guest);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        uint16_t port;
        memcpy(&port, data + 2, 2);
        uint32_t ip;
        memcpy(&ip, data + 4, 4);
        uint32_t realIp = ip;
        uint16_t base = g_base;
        if (!RealFromSecure(ip, realIp, base))
            base = g_base;
        address.sin_addr.s_addr = realIp;
        address.sin_port = htons(ToHostPort(ntohs(port), base));
        return address;
    }

    static void WriteAddress(uint32_t guest, uint32_t guestLength, const sockaddr_in& address)
    {
        if (guest == 0)
            return;
        uint8_t* data = g_memory.Translate<uint8_t>(guest);
        memset(data, 0, 16);
        *reinterpret_cast<be<uint16_t>*>(data) = uint16_t(AF_INET);
        uint16_t hostPort = ntohs(address.sin_port);
        uint16_t base = hostPort & ~uint16_t(PORT_BLOCK - 1);
        uint16_t offset = hostPort & (PORT_BLOCK - 1);
        uint16_t titlePort = g_titlePorts[offset] != 0 ? g_titlePorts[offset] : uint16_t(1000 + offset);
        uint16_t networkPort = htons(titlePort);
        memcpy(data + 2, &networkPort, 2);
        uint32_t secure = PeerByReal(address.sin_addr.s_addr, base);
        memcpy(data + 4, &secure, 4);
        if (guestLength != 0)
            *g_memory.Translate<be<uint32_t>>(guestLength) = 16;
    }
}

namespace imports
{
    // AR_ONLINE_TRACE: the traffic, the first bytes of each packet, up to a limit.
    template <typename... Args>
    static void Trace(fmt::format_string<Args...> format, Args&&... args)
    {
        static const bool enabled = AR_ENV("AR_ONLINE_TRACE") != nullptr;
        static std::atomic<int> left{ 400 };
        if (enabled && left.fetch_sub(1) > 0)
            LOGF_INFO("net: {}", fmt::format(format, std::forward<Args>(args)...));
    }

    // AR_ONLINE_TRACE: how much goes through each socket, every 5 seconds.
    static void CountTraffic(uint32_t socket, bool sending, int size)
    {
        static const bool enabled = AR_ENV("AR_ONLINE_TRACE") != nullptr;
        if (!enabled || size < 0)
            return;
        static std::mutex mutex;
        struct Counts
        {
            uint32_t sent = 0, received = 0, sentBytes = 0, receivedBytes = 0;
        };
        static std::map<uint32_t, Counts> counts;
        static auto last = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        Counts& entry = counts[socket];
        (sending ? entry.sent : entry.received)++;
        (sending ? entry.sentBytes : entry.receivedBytes) += uint32_t(size);
        auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(5))
        {
            last = now;
            std::string text;
            for (const auto& [id, c] : counts)
                text += fmt::format(" [{}: sent {} ({}) received {} ({})]", id, c.sent, c.sentBytes, c.received, c.receivedBytes);
            LOGF_INFO("traffic:{}", text);
            counts.clear();
        }
    }

    static std::string Bytes(const void* data, int size)
    {
        std::string text;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (int i = 0; i < std::min(size, 24); i++)
            text += fmt::format("{:02X}", bytes[i]);
        return text;
    }

    static int32_t Fail(uint32_t error = X_WSAENETDOWN)
    {
        t_lastSocketError = error;
        return X_SOCKET_ERROR;
    }

    static int32_t FailFromHost()
    {
        return Fail(uint32_t(WSAGetLastError()));
    }

    // Most functions take a caller type first (title or system).
    uint32_t NetDll_WSAStartup(uint32_t caller, uint32_t version, uint8_t* data)
    {
        // WSADATA: wVersion, wHighVersion, szDescription[257], szSystemStatus[129], iMaxSockets, iMaxUdpDg, lpVendorInfo.
        if (data != nullptr)
        {
            memset(data, 0, 0x190);
            *reinterpret_cast<be<uint16_t>*>(data + 0) = uint16_t(version);
            *reinterpret_cast<be<uint16_t>*>(data + 2) = 0x0202;
        }

        return 0;
    }

    uint32_t NetDll_WSACleanup(uint32_t caller)
    {
        return 0;
    }

    uint32_t NetDll_WSAGetLastError()
    {
        return t_lastSocketError;
    }

    uint32_t NetDll_XNetStartup(uint32_t caller, uint32_t parameters)
    {
        return 0;
    }

    uint32_t NetDll_XNetGetTitleXnAddr(uint32_t caller, uint8_t* address)
    {
        if (net::Online())
        {
            if (address != nullptr)
                net::FillLocalAddress(address);
            return 0x00000002 | 0x00000004 | 0x00000010 | 0x00000040 | 0x00000080; // Ethernet, static, gateway, DNS and online.
        }

        // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
        if (address != nullptr)
            memset(address, 0, 0x24);

        return XNET_GET_XNADDR_NONE;
    }

    int32_t NetDll_socket(uint32_t caller, uint32_t family, uint32_t type, uint32_t protocol)
    {
        if (!net::Online())
            return Fail();
        net::LocalBase();
        // VDP (protocol 254) is UDP with a header of the title's own making.
        bool datagram = type == 2;
        SOCKET host = ::socket(AF_INET, datagram ? SOCK_DGRAM : SOCK_STREAM, datagram ? IPPROTO_UDP : IPPROTO_TCP);
        if (host == INVALID_SOCKET)
            return FailFromHost();
        std::lock_guard lock(net::g_socketsMutex);
        uint32_t handle = net::g_nextSocket++;
        net::g_sockets[handle] = { host, datagram };
        LOGF_INFO("online: socket {} type {} protocol {}", handle, type, protocol);
        return int32_t(handle);
    }

    int32_t NetDll_closesocket(uint32_t caller, uint32_t socket)
    {
        if (!net::Online())
            return 0;
        SOCKET host = INVALID_SOCKET;
        {
            std::lock_guard lock(net::g_socketsMutex);
            auto it = net::g_sockets.find(socket);
            if (it != net::g_sockets.end())
            {
                host = it->second.host;
                net::g_sockets.erase(it);
            }
        }
        if (host != INVALID_SOCKET)
            closesocket(host);
        return 0;
    }

    int32_t NetDll_shutdown(uint32_t caller, uint32_t socket, uint32_t how)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        return ::shutdown(guest.host, int(how)) == 0 ? 0 : FailFromHost();
    }

    int32_t NetDll_ioctlsocket(uint32_t caller, uint32_t socket, uint32_t command, uint32_t argument)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        auto* value = g_memory.Translate<be<uint32_t>>(argument);
        u_long host = value->get();
        if (::ioctlsocket(guest.host, long(command), &host) != 0)
            return FailFromHost();
        *value = uint32_t(host);
        return 0;
    }

    int32_t NetDll_setsockopt(uint32_t caller, uint32_t socket, uint32_t level, uint32_t option, uint32_t value, uint32_t length)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        // Most options are 32 bit values. Options only the console has just succeed.
        if (level == 0xFFFF && length == 4)
        {
            int number = int(g_memory.Translate<be<uint32_t>>(value)->get());
            ::setsockopt(guest.host, SOL_SOCKET, int(option), reinterpret_cast<const char*>(&number), sizeof(number));
        }
        return 0;
    }

    int32_t NetDll_bind(uint32_t caller, uint32_t socket, uint32_t name, uint32_t length)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        uint16_t base = net::LocalBase();
        const uint8_t* data = g_memory.Translate<const uint8_t>(name);
        uint16_t port;
        memcpy(&port, data + 2, 2);
        uint16_t titlePort = ntohs(port);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        uint16_t offset = titlePort & (net::PORT_BLOCK - 1);
        address.sin_port = htons(uint16_t(base + offset));
        net::g_titlePorts[offset] = titlePort;
        LOGF_INFO("online: bind socket {} title port {} to {}", socket, titlePort, base + offset);
        if (::bind(guest.host, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            return FailFromHost();
        return 0;
    }

    int32_t NetDll_connect(uint32_t caller, uint32_t socket, uint32_t name, uint32_t length)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        sockaddr_in address = net::ReadAddress(name);
        int connected = ::connect(guest.host, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        Trace("connect socket {} to port {}: {} error {}", socket, ntohs(address.sin_port), connected, connected != 0 ? WSAGetLastError() : 0);
        return connected == 0 ? 0 : FailFromHost();
    }

    int32_t NetDll_listen(uint32_t caller, uint32_t socket, uint32_t backlog)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        Trace("listen socket {}", socket);
        return ::listen(guest.host, int(backlog)) == 0 ? 0 : FailFromHost();
    }

    int32_t NetDll_accept(uint32_t caller, uint32_t socket, uint32_t address, uint32_t length)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        sockaddr_in from{};
        int size = sizeof(from);
        SOCKET host = ::accept(guest.host, reinterpret_cast<sockaddr*>(&from), &size);
        if (host == INVALID_SOCKET)
            return FailFromHost();
        Trace("accept socket {} from port {}", socket, ntohs(from.sin_port));
        net::WriteAddress(address, length, from);
        std::lock_guard lock(net::g_socketsMutex);
        uint32_t handle = net::g_nextSocket++;
        net::g_sockets[handle] = { host, false };
        return int32_t(handle);
    }

    // fd_set on the guest: a count and handles, 32 bits each.
    static void ReadSet(uint32_t guest, fd_set& set, std::vector<std::pair<uint32_t, SOCKET>>& list)
    {
        FD_ZERO(&set);
        list.clear();
        if (guest == 0)
            return;
        auto* words = g_memory.Translate<be<uint32_t>>(guest);
        uint32_t count = std::min<uint32_t>(words[0], 64);
        for (uint32_t i = 0; i < count; i++)
        {
            auto socket = net::FindSocket(words[1 + i]);
            if (socket.host != INVALID_SOCKET)
            {
                FD_SET(socket.host, &set);
                list.push_back({ words[1 + i], socket.host });
            }
        }
    }

    static void WriteSet(uint32_t guest, const fd_set& set, const std::vector<std::pair<uint32_t, SOCKET>>& list)
    {
        if (guest == 0)
            return;
        auto* words = g_memory.Translate<be<uint32_t>>(guest);
        uint32_t count = 0;
        for (const auto& [handle, host] : list)
        {
            if (FD_ISSET(host, &set))
                words[1 + count++] = handle;
        }
        words[0] = count;
    }

    int32_t NetDll_select(uint32_t caller, uint32_t count, uint32_t read, uint32_t write, uint32_t except, uint32_t timeout)
    {
        if (!net::Online())
            return Fail();
        fd_set readSet, writeSet, exceptSet;
        std::vector<std::pair<uint32_t, SOCKET>> readList, writeList, exceptList;
        ReadSet(read, readSet, readList);
        ReadSet(write, writeSet, writeList);
        ReadSet(except, exceptSet, exceptList);
        if (readList.empty() && writeList.empty() && exceptList.empty())
            return 0;
        timeval host{}, * hostTimeout = nullptr;
        if (timeout != 0)
        {
            auto* words = g_memory.Translate<be<uint32_t>>(timeout);
            host.tv_sec = long(words[0].get());
            host.tv_usec = long(words[1].get());
            hostTimeout = &host;
        }
        int result = ::select(0, &readSet, &writeSet, &exceptSet, hostTimeout);
        if (result == SOCKET_ERROR)
            return FailFromHost();
        WriteSet(read, readSet, readList);
        WriteSet(write, writeSet, writeList);
        WriteSet(except, exceptSet, exceptList);
        return result;
    }

    int32_t NetDll_recv(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        int result = ::recv(guest.host, g_memory.Translate<char>(buffer), int(length), int(flags));
        if (result != SOCKET_ERROR)
        {
            CountTraffic(socket, false, result);
            Trace("recv socket {} {} bytes {}", socket, result, Bytes(g_memory.Translate<char>(buffer), result));
        }
        return result == SOCKET_ERROR ? FailFromHost() : result;
    }

    int32_t NetDll_recvfrom(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from, uint32_t fromLength)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        sockaddr_in address{};
        int size = sizeof(address);
        int result = ::recvfrom(guest.host, g_memory.Translate<char>(buffer), int(length), int(flags), reinterpret_cast<sockaddr*>(&address), &size);
        if (result == SOCKET_ERROR)
            return FailFromHost();
        CountTraffic(socket, false, result);
        Trace("recvfrom socket {} from port {} {} bytes {}", socket, ntohs(address.sin_port), result, Bytes(g_memory.Translate<char>(buffer), result));
        net::WriteAddress(from, fromLength, address);
        return result;
    }

    int32_t NetDll_send(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        int result = ::send(guest.host, g_memory.Translate<const char>(buffer), int(length), int(flags));
        CountTraffic(socket, true, result);
        Trace("send socket {} {} of {} bytes {}", socket, result, length, Bytes(g_memory.Translate<char>(buffer), int(length)));
        return result == SOCKET_ERROR ? FailFromHost() : result;
    }

    int32_t NetDll_sendto(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to, uint32_t toLength)
    {
        auto guest = net::FindSocket(socket);
        if (guest.host == INVALID_SOCKET)
            return Fail();
        sockaddr_in address = net::ReadAddress(to);
        int result = ::sendto(guest.host, g_memory.Translate<const char>(buffer), int(length), int(flags), reinterpret_cast<sockaddr*>(&address), sizeof(address));
        CountTraffic(socket, true, result);
        const uint8_t* toBytes = g_memory.Translate<const uint8_t>(to);
        Trace("sendto socket {} guest {}.{}.{}.{} port {} {} of {} bytes {}", socket, toBytes[4], toBytes[5], toBytes[6], toBytes[7], ntohs(address.sin_port), result, length, Bytes(g_memory.Translate<char>(buffer), int(length)));
        return result == SOCKET_ERROR ? FailFromHost() : result;
    }

    uint32_t NetDll___WSAFDIsSet(uint32_t socket, uint32_t set)
    {
        if (set == 0)
            return 0;
        auto* words = g_memory.Translate<be<uint32_t>>(set);
        for (uint32_t i = 0; i < std::min<uint32_t>(words[0], 64); i++)
        {
            if (words[1 + i] == socket)
                return 1;
        }
        return 0;
    }

    uint32_t NetDll_inet_addr(const char* text)
    {
        // Returns the address in network byte order, which is the natural value on a big endian machine.
        unsigned parts[4];
        if (text == nullptr || sscanf(text, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4)
            return 0xFFFFFFFF; // INADDR_NONE

        return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    }

    // Secure network addresses: another copy's address given by its session.
    uint32_t NetDll_XNetXnAddrToInAddr(uint32_t caller, uint32_t xnaddr, uint32_t keyId, uint32_t inaddr)
    {
        if (!net::Online())
            return X_WSAEINVAL;
        const uint8_t* address = g_memory.Translate<const uint8_t>(xnaddr);
        uint32_t ip;
        uint16_t port;
        memcpy(&ip, address, 4);
        memcpy(&port, address + 8, 2);
        uint32_t secure = net::PeerByReal(ip, ntohs(port));
        {
            std::lock_guard lock(net::g_peersMutex);
            size_t index = (ntohl(secure) & 0xFFFFFF) - 1;
            memcpy(net::g_peers[index].xnaddr, address, 36);
            net::g_peers[index].hasAddress = true;
        }
        memcpy(g_memory.Translate<uint8_t>(inaddr), &secure, 4);
        return 0;
    }

    uint32_t NetDll_XNetInAddrToXnAddr(uint32_t caller, uint32_t inaddr, uint32_t xnaddr, uint32_t keyId)
    {
        if (!net::Online())
            return X_WSAEINVAL;
        uint32_t secure = ByteSwap(inaddr);
        std::lock_guard lock(net::g_peersMutex);
        size_t index = (ntohl(secure) & 0xFFFFFF) - 1;
        if ((ntohl(secure) & 0xFF000000) != 0x01000000 || index >= net::g_peers.size())
            return X_WSAEINVAL;
        const net::Peer& peer = net::g_peers[index];
        uint8_t* out = g_memory.Translate<uint8_t>(xnaddr);
        if (peer.hasAddress)
            memcpy(out, peer.xnaddr, 36);
        else
        {
            memset(out, 0, 36);
            memcpy(out, &peer.ip, 4);
            memcpy(out + 4, &peer.ip, 4);
            uint16_t port = htons(peer.base);
            memcpy(out + 8, &port, 2);
        }
        if (keyId != 0)
            memset(g_memory.Translate<uint8_t>(keyId), 0, 8);
        return 0;
    }

    uint32_t NetDll_XNetUnregisterInAddr(uint32_t caller, uint32_t inaddr)
    {
        return 0;
    }

    uint32_t NetDll_XNetXnAddrToMachineId(uint32_t caller, uint32_t xnaddr, uint32_t machineId)
    {
        if (!net::Online())
            return X_WSAEINVAL;
        // Different per copy, from its address.
        const uint8_t* address = g_memory.Translate<const uint8_t>(xnaddr);
        uint64_t id = 0xCBF29CE484222325ull;
        for (int i = 8; i < 36; i++)
            id = (id ^ address[i]) * 0x100000001B3ull;
        *g_memory.Translate<be<uint64_t>>(machineId) = id | 0xFA00000000000000ull;
        return 0;
    }

    // Quality of service probes: every copy answers, as fast as can be, since there's nothing to measure them against.
    uint32_t NetDll_XNetQosListen(uint32_t caller, uint32_t keyId, uint32_t data, uint32_t size, uint32_t bitsPerSecond, uint32_t flags)
    {
        return net::Online() ? 0 : X_WSAENETDOWN;
    }

    // What was handed out, as the title also releases things that didn't come from here (seen from Custom Match).
    static std::mutex g_qosMutex;
    static std::set<uint32_t> g_qosBlocks;

    uint32_t NetDll_XNetQosLookup(uint32_t caller, uint32_t count, uint32_t xnaddrs, uint32_t keyIds, uint32_t keys,
        uint32_t securityGatewayCount, uint32_t securityGateways, uint32_t serviceIds, uint32_t probes, uint32_t bitsPerSecond, uint32_t flags, uint32_t event, uint32_t result)
    {
        if (!net::Online())
            return X_WSAENETDOWN;
        // XNQOS: cxnqos, cxnqosPending, then XNQOSINFO for each (24 bytes): flags, reserved, probes sent, received, data size, data pointer, round trip min and median, up and down rate.
        uint32_t size = 8 + std::max<uint32_t>(count, 1) * 24;
        uint32_t guest = g_runtimeHeap.AllocGuest(size);
        uint8_t* block = g_memory.Translate<uint8_t>(guest);
        memset(block, 0, size);
        *reinterpret_cast<be<uint32_t>*>(block) = count;
        for (uint32_t i = 0; i < count; i++)
        {
            uint8_t* info = block + 8 + i * 24;
            info[0] = 0x01 | 0x02 | 0x08; // Target contacted, complete, partial data.
            info[2] = 8;
            info[3] = 8;
            *reinterpret_cast<be<uint16_t>*>(info + 12) = 10;
            *reinterpret_cast<be<uint16_t>*>(info + 14) = 10;
            *reinterpret_cast<be<uint32_t>*>(info + 16) = 10000000;
            *reinterpret_cast<be<uint32_t>*>(info + 20) = 10000000;
        }
        {
            std::lock_guard lock(g_qosMutex);
            g_qosBlocks.insert(guest);
        }
        LOGF_INFO("online: quality of service lookup of {} addresses: {:08X}, kept at {:08X}", count, guest, result);
        if (result != 0)
            *g_memory.Translate<be<uint32_t>>(result) = guest;
        return 0;
    }

    uint32_t NetDll_XNetQosRelease(uint32_t caller, uint32_t qos)
    {
        if (!net::Online() || qos == 0)
            return 0;
        std::lock_guard lock(g_qosMutex);
        if (g_qosBlocks.erase(qos) != 0)
            g_runtimeHeap.FreeGuest(qos);
        else
            LOGF_WARNING("online: quality of service release of {:08X}, which didn't come from here", qos);
        return 0;
    }
}

AR_IMPORT(NetDll_WSAStartup);
AR_IMPORT(NetDll_WSACleanup);
AR_IMPORT(NetDll_WSAGetLastError);
AR_IMPORT(NetDll_XNetStartup);
AR_IMPORT(NetDll_XNetGetTitleXnAddr);
AR_IMPORT(NetDll_socket);
AR_IMPORT(NetDll_closesocket);
AR_IMPORT(NetDll_shutdown);
AR_IMPORT(NetDll_ioctlsocket);
AR_IMPORT(NetDll_setsockopt);
AR_IMPORT(NetDll_bind);
AR_IMPORT(NetDll_connect);
AR_IMPORT(NetDll_listen);
AR_IMPORT(NetDll_accept);
AR_IMPORT(NetDll_select);
AR_IMPORT(NetDll_recv);
AR_IMPORT(NetDll_recvfrom);
AR_IMPORT(NetDll_send);
AR_IMPORT(NetDll_sendto);
AR_IMPORT(NetDll___WSAFDIsSet);
AR_IMPORT(NetDll_inet_addr);
AR_IMPORT(NetDll_XNetXnAddrToInAddr);
AR_IMPORT(NetDll_XNetInAddrToXnAddr);
AR_IMPORT(NetDll_XNetUnregisterInAddr);
AR_IMPORT(NetDll_XNetXnAddrToMachineId);
AR_IMPORT(NetDll_XNetQosListen);
AR_IMPORT(NetDll_XNetQosLookup);
AR_IMPORT(NetDll_XNetQosRelease);
