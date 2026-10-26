// Networking. The online services the game used are gone, so the console behaves as if it has no network
// connection: startup succeeds, there is no network address and socket operations fail with WSAENETDOWN.
#include <stdafx.h>
#include <kernel/imports.h>

constexpr int32_t X_SOCKET_ERROR = -1;
constexpr uint32_t X_WSAEINVAL = 10022;
constexpr uint32_t X_WSAENETDOWN = 10050;
constexpr uint32_t X_WSANOTINITIALISED = 10093;
constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;

static thread_local uint32_t t_lastSocketError;

namespace imports
{
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
        // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
        if (address != nullptr)
            memset(address, 0, 0x24);

        return XNET_GET_XNADDR_NONE;
    }

    static int32_t Fail(uint32_t error = X_WSAENETDOWN)
    {
        t_lastSocketError = error;
        return X_SOCKET_ERROR;
    }

    int32_t NetDll_socket(uint32_t caller, uint32_t family, uint32_t type, uint32_t protocol) { return Fail(); }
    int32_t NetDll_closesocket(uint32_t caller, uint32_t socket) { return 0; }
    int32_t NetDll_shutdown(uint32_t caller, uint32_t socket, uint32_t how) { return Fail(); }
    int32_t NetDll_ioctlsocket(uint32_t caller, uint32_t socket, uint32_t command, uint32_t argument) { return Fail(); }
    int32_t NetDll_setsockopt(uint32_t caller, uint32_t socket, uint32_t level, uint32_t option, uint32_t value, uint32_t length) { return Fail(); }
    int32_t NetDll_bind(uint32_t caller, uint32_t socket, uint32_t name, uint32_t length) { return Fail(); }
    int32_t NetDll_connect(uint32_t caller, uint32_t socket, uint32_t name, uint32_t length) { return Fail(); }
    int32_t NetDll_listen(uint32_t caller, uint32_t socket, uint32_t backlog) { return Fail(); }
    int32_t NetDll_accept(uint32_t caller, uint32_t socket, uint32_t address, uint32_t length) { return Fail(); }
    int32_t NetDll_select(uint32_t caller, uint32_t count, uint32_t read, uint32_t write, uint32_t except, uint32_t timeout) { return Fail(); }
    int32_t NetDll_recv(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags) { return Fail(); }
    int32_t NetDll_recvfrom(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from, uint32_t fromLength) { return Fail(); }
    int32_t NetDll_send(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags) { return Fail(); }
    int32_t NetDll_sendto(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to, uint32_t toLength) { return Fail(); }

    uint32_t NetDll___WSAFDIsSet(uint32_t socket, uint32_t set)
    {
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

    // Secure network addresses and quality of service probes need the Xbox Live infrastructure.
    uint32_t NetDll_XNetXnAddrToInAddr(uint32_t caller, uint32_t xnaddr, uint32_t keyId, uint32_t inaddr) { return X_WSAEINVAL; }
    uint32_t NetDll_XNetInAddrToXnAddr(uint32_t caller, uint32_t inaddr, uint32_t xnaddr, uint32_t keyId) { return X_WSAEINVAL; }
    uint32_t NetDll_XNetUnregisterInAddr(uint32_t caller, uint32_t inaddr) { return 0; }
    uint32_t NetDll_XNetXnAddrToMachineId(uint32_t caller, uint32_t xnaddr, uint32_t machineId) { return X_WSAEINVAL; }
    uint32_t NetDll_XNetQosListen(uint32_t caller, uint32_t keyId, uint32_t data, uint32_t size, uint32_t bitsPerSecond, uint32_t flags) { return X_WSAENETDOWN; }
    uint32_t NetDll_XNetQosLookup(uint32_t caller, uint32_t count, uint32_t xnaddrs, uint32_t keyIds, uint32_t keys,
        uint32_t securityGatewayCount, uint32_t securityGateways, uint32_t serviceIds) { return X_WSAENETDOWN; }
    uint32_t NetDll_XNetQosRelease(uint32_t caller, uint32_t qos) { return 0; }
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
