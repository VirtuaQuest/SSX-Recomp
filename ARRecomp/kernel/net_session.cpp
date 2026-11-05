// Experimental multiplayer between copies of the game, with no online service (AR_ONLINE): the session messages the title sends to Xbox Live are answered here.
// A copy hosting a game answers other copies asking for one on its discovery port (see LANs and AR_ONLINE_PEER). Layouts from Xenia's xgi_app.cc.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/net_session.h>
#include <random>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace net
{
    // XSESSION_INFO: session id (8), host address (XNADDR, 36), key exchange key (16).
    static constexpr uint32_t SESSION_INFO_SIZE = 60;
    static constexpr uint32_t XSESSION_CREATE_HOST = 0x00000001;

    struct Hosted
    {
        bool active = false;
        uint8_t info[SESSION_INFO_SIZE] = {};
        uint32_t flags = 0, publicSlots = 0, privateSlots = 0, filledPublic = 1, filledPrivate = 0;
    };
    static std::mutex g_hostedMutex;
    static Hosted g_hosted;
    // What the title has said about this player (game type, map and so on), to go with a game hosted here: contexts (id, value), properties (id, data).
    static std::map<uint32_t, uint32_t> g_contexts;
    static std::map<uint32_t, std::vector<uint8_t>> g_properties;
    static std::string g_hostName = "Player";

    static constexpr char QUERY[4] = { 'A', 'R', 'M', 'Q' }, REPLY[4] = { 'A', 'R', 'M', 'R' };
    static constexpr uint32_t PROPERTY_HOST_NAME = 0x40008109;

    void SetHostName(const char* name)
    {
        std::lock_guard lock(g_hostedMutex);
        g_hostName = name;
    }

    // A reply: magic, session info, four numbers, host name (16), then counts and values of contexts and properties. All big endian.
    static std::vector<uint8_t> BuildReply()
    {
        std::vector<uint8_t> reply;
        auto put = [&](const void* data, size_t size)
            {
                const uint8_t* bytes = static_cast<const uint8_t*>(data);
                reply.insert(reply.end(), bytes, bytes + size);
            };
        auto putWord = [&](uint32_t value)
            {
                uint32_t big = htonl(value);
                put(&big, 4);
            };
        put(REPLY, 4);
        put(g_hosted.info, SESSION_INFO_SIZE);
        putWord(g_hosted.publicSlots);
        putWord(g_hosted.privateSlots);
        putWord(g_hosted.filledPublic);
        putWord(g_hosted.filledPrivate);
        char name[16] = {};
        strncpy(name, g_hostName.c_str(), 15);
        put(name, 16);
        putWord(uint32_t(g_contexts.size()));
        for (const auto& [id, value] : g_contexts)
        {
            putWord(id);
            putWord(value);
        }
        putWord(uint32_t(g_properties.size()));
        for (const auto& [id, data] : g_properties)
        {
            putWord(id);
            putWord(uint32_t(data.size()));
            put(data.data(), data.size());
        }
        return reply;
    }

    void StartDiscovery(uint64_t socket)
    {
        std::thread([socket]
            {
                SOCKET s = SOCKET(socket);
                char packet[64];
                while (true)
                {
                    sockaddr_in from{};
                    int size = sizeof(from);
                    int got = ::recvfrom(s, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &size);
                    if (got < 4 || memcmp(packet, QUERY, 4) != 0)
                        continue;
                    std::lock_guard lock(g_hostedMutex);
                    if (!g_hosted.active)
                        continue;
                    std::vector<uint8_t> reply = BuildReply();
                    ::sendto(s, reinterpret_cast<const char*>(reply.data()), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
                    LOGF_INFO("online: told another copy about the game hosted here");
                }
            }).detach();
    }

    // A game another copy told us about.
    struct Found
    {
        uint8_t info[SESSION_INFO_SIZE];
        uint32_t publicSlots, privateSlots, filledPublic, filledPrivate;
        char hostName[16];
        std::vector<std::pair<uint32_t, uint32_t>> contexts;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> properties;
    };

    static bool ParseReply(const uint8_t* data, size_t size, Found& game)
    {
        size_t at = 4;
        auto word = [&](uint32_t& value)
            {
                if (at + 4 > size)
                    return false;
                uint32_t big;
                memcpy(&big, data + at, 4);
                value = ntohl(big);
                at += 4;
                return true;
            };
        if (size < 4 + SESSION_INFO_SIZE + 16 + 16 + 8 || memcmp(data, REPLY, 4) != 0)
            return false;
        memcpy(game.info, data + at, SESSION_INFO_SIZE);
        at += SESSION_INFO_SIZE;
        if (!word(game.publicSlots) || !word(game.privateSlots) || !word(game.filledPublic) || !word(game.filledPrivate))
            return false;
        memcpy(game.hostName, data + at, 16);
        game.hostName[15] = 0;
        at += 16;
        uint32_t count;
        if (!word(count) || count > 64)
            return false;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t id, value;
            if (!word(id) || !word(value))
                return false;
            game.contexts.push_back({ id, value });
        }
        if (!word(count) || count > 64)
            return false;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t id, length;
            if (!word(id) || !word(length) || at + length > size)
                return false;
            game.properties.push_back({ id, std::vector<uint8_t>(data + at, data + at + length) });
            at += length;
        }
        return true;
    }

    // Asks every copy it can think of: on this machine, on the local network, and at the addresses in AR_ONLINE_PEER (comma separated).
    static std::vector<Found> FindGames()
    {
        std::vector<Found> found;
        LocalBase();
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL on = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        std::vector<uint32_t> addresses = { htonl(INADDR_LOOPBACK), htonl(INADDR_BROADCAST) };
        if (const char* text = AR_ENV("AR_ONLINE_PEER"))
        {
            std::string list = text;
            size_t start = 0;
            while (start <= list.size())
            {
                size_t end = std::min(list.find(',', start), list.size());
                uint32_t address;
                if (inet_pton(AF_INET, list.substr(start, end - start).c_str(), &address) == 1)
                    addresses.push_back(address);
                start = end + 1;
            }
        }
        auto ask = [&]
            {
                for (uint32_t address : addresses)
                {
                    for (uint16_t i = 0; i < MAX_INSTANCES; i++)
                    {
                        sockaddr_in to{};
                        to.sin_family = AF_INET;
                        to.sin_addr.s_addr = address;
                        to.sin_port = htons(uint16_t(BASE_PORT + i * PORT_BLOCK + DISCOVERY_OFFSET));
                        ::sendto(s, QUERY, 4, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                    }
                }
            };
        ask();
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < until)
        {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            timeval wait{ 0, 50000 };
            if (::select(0, &set, nullptr, nullptr, &wait) <= 0)
                continue;
            uint8_t reply[1400];
            sockaddr_in from{};
            int fromSize = sizeof(from);
            int got = ::recvfrom(s, reinterpret_cast<char*>(reply), sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
            Found game;
            if (got <= 0 || !ParseReply(reply, size_t(got), game))
                continue;
            // Where it really answered from: a host behind a router only knows its address there. (Not the loopback: that's this machine, and others may join it.)
            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            {
                memcpy(game.info + 8, &from.sin_addr.s_addr, 4);
                memcpy(game.info + 12, &from.sin_addr.s_addr, 4);
            }
            bool seen = false;
            for (const Found& other : found)
                seen |= memcmp(other.info, game.info, 8) == 0;
            if (!seen)
                found.push_back(game);
        }
        closesocket(s);
        return found;
    }

    // XSessionSearchEx: procedure, user, results wanted, property and context counts, properties, contexts, size of the results buffer, the buffer.
    // XSESSION_SEARCHRESULT_HEADER: count, pointer. XSESSION_SEARCHRESULT: info, open public and private slots, filled public and private slots, property
    // and context counts and pointers. XUSER_PROPERTY (24 bytes): id, then X_USER_DATA: type at +8, value at +16 (a number, or size and pointer).
    static void WriteSearchResults(const be<uint32_t>* words)
    {
        uint32_t wanted = words[2], counts = words[3], properties = words[4], contexts = words[5], size = words[6], results = words[7];
        uint32_t searchProperties = counts >> 16, searchContexts = counts & 0xFFFF;
        static constexpr uint32_t RESULT_SIZE = 92, PROPERTY_SIZE = 0x18, CONTEXT_SIZE = 8;
        std::vector<Found> games = FindGames();
        uint8_t* out = g_memory.Translate<uint8_t>(results);
        memset(out, 0, size);

        // The search's own values come first in each result, what the host says replaces them.
        std::vector<std::pair<uint32_t, std::array<uint8_t, PROPERTY_SIZE>>> asked;
        for (uint32_t i = 0; i < searchProperties; i++)
        {
            std::array<uint8_t, PROPERTY_SIZE> entry;
            memcpy(entry.data(), g_memory.Translate<uint8_t>(properties + i * PROPERTY_SIZE), PROPERTY_SIZE);
            uint32_t id = *reinterpret_cast<be<uint32_t>*>(entry.data());
            asked.push_back({ id, entry });
        }
        std::vector<std::pair<uint32_t, uint32_t>> askedContexts;
        for (uint32_t i = 0; i < searchContexts; i++)
        {
            auto* entry = g_memory.Translate<be<uint32_t>>(contexts + i * CONTEXT_SIZE);
            askedContexts.push_back({ entry[0], entry[1] });
        }

        uint32_t count = std::min(uint32_t(games.size()), wanted);
        uint32_t limit = results + size;
        uint32_t extra = results + 8 + count * RESULT_SIZE;
        uint32_t written = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            Found& game = games[i];
            // Contexts: as asked, then the host's.
            std::map<uint32_t, uint32_t> allContexts;
            for (const auto& [id, value] : askedContexts)
                allContexts[id] = value;
            for (const auto& [id, value] : game.contexts)
                allContexts[id] = value;
            std::map<uint32_t, std::array<uint8_t, PROPERTY_SIZE>> allProperties;
            for (const auto& [id, entry] : asked)
                allProperties[id] = entry;
            uint32_t needed = uint32_t(allContexts.size()) * CONTEXT_SIZE + uint32_t(asked.size() + game.properties.size() + 1) * PROPERTY_SIZE + 64;
            for (const auto& [id, data] : game.properties)
                needed += uint32_t(data.size()) + 4;
            if (extra + needed > limit)
                break;

            auto addData = [&](const void* data, uint32_t length)
                {
                    uint32_t guest = extra;
                    memcpy(g_memory.Translate<uint8_t>(guest), data, length);
                    extra += (length + 3) & ~3u;
                    return guest;
                };
            auto setProperty = [&](uint32_t id, const void* data, uint32_t length)
                {
                    std::array<uint8_t, PROPERTY_SIZE> entry{};
                    *reinterpret_cast<be<uint32_t>*>(entry.data()) = id;
                    uint8_t type = uint8_t(id >> 28);
                    entry[8] = type;
                    if (type == 4 || type == 6) // Text and binary: size and pointer.
                    {
                        *reinterpret_cast<be<uint32_t>*>(entry.data() + 16) = length;
                        *reinterpret_cast<be<uint32_t>*>(entry.data() + 20) = addData(data, length);
                    }
                    else // A number, as the title stored it.
                        memcpy(entry.data() + 16, data, std::min<uint32_t>(length, 8));
                    allProperties[id] = entry;
                };
            for (const auto& [id, data] : game.properties)
                setProperty(id, data.data(), uint32_t(data.size()));
            // The host's name, in big endian UTF-16.
            uint16_t nameText[16] = {};
            uint32_t nameLength = 0;
            for (; nameLength < 15 && game.hostName[nameLength] != 0; nameLength++)
                nameText[nameLength] = ByteSwap(uint16_t(uint8_t(game.hostName[nameLength])));
            setProperty(PROPERTY_HOST_NAME, nameText, (nameLength + 1) * 2);

            uint8_t* entry = out + 8 + i * RESULT_SIZE;
            memcpy(entry, game.info, SESSION_INFO_SIZE);
            auto* numbers = reinterpret_cast<be<uint32_t>*>(entry + SESSION_INFO_SIZE);
            numbers[0] = game.publicSlots > game.filledPublic ? game.publicSlots - game.filledPublic : 0;
            numbers[1] = game.privateSlots > game.filledPrivate ? game.privateSlots - game.filledPrivate : 0;
            numbers[2] = game.filledPublic;
            numbers[3] = game.filledPrivate;
            numbers[4] = uint32_t(allProperties.size());
            numbers[5] = uint32_t(allContexts.size());
            numbers[6] = extra;
            for (const auto& [id, property] : allProperties)
            {
                memcpy(g_memory.Translate<uint8_t>(extra), property.data(), PROPERTY_SIZE);
                extra += PROPERTY_SIZE;
            }
            numbers[7] = extra;
            for (const auto& [id, value] : allContexts)
            {
                auto* pair = g_memory.Translate<be<uint32_t>>(extra);
                pair[0] = id;
                pair[1] = value;
                extra += CONTEXT_SIZE;
            }
            LOGF_INFO("online: found the game of {}: {} properties, {} contexts", game.hostName, allProperties.size(), allContexts.size());
            written++;
        }
        *reinterpret_cast<be<uint32_t>*>(out) = written;
        *reinterpret_cast<be<uint32_t>*>(out + 4) = written != 0 ? results + 8 : 0;
        LOGF_INFO("online: search found {} games, reported {}", games.size(), written);
    }

    static std::atomic<int> g_openSessions;

    bool InSession()
    {
        return g_openSessions.load(std::memory_order_relaxed) > 0;
    }

    bool HandleSessionMessage(uint32_t message, uint32_t buffer, uint32_t bufferLength, uint32_t& result)
    {
        if (!Online())
            return false;
        auto* words = buffer != 0 ? g_memory.Translate<be<uint32_t>>(buffer) : nullptr;
        result = X_ERROR_SUCCESS;
        switch (message)
        {
        case 0x000B0010: // XSessionCreate: session, flags, public slots, private slots, user, info, nonce.
        {
            uint32_t flags = words[1], publicSlots = words[2], privateSlots = words[3], infoAddress = words[5], nonceAddress = words[6];
            LOGF_INFO("online: session create, flags {:08X}, slots {} and {}", flags, publicSlots, privateSlots);
            g_openSessions.fetch_add(1, std::memory_order_relaxed);
            std::mt19937_64 random{ std::random_device{}() };
            if (nonceAddress != 0 && (flags & XSESSION_CREATE_HOST))
                *g_memory.Translate<be<uint64_t>>(nonceAddress) = random();
            if (infoAddress != 0 && (flags & XSESSION_CREATE_HOST))
            {
                uint8_t* info = g_memory.Translate<uint8_t>(infoAddress);
                memset(info, 0, SESSION_INFO_SIZE);
                uint64_t id = random();
                memcpy(info, &id, 8);
                FillLocalAddress(info + 8);
                uint64_t key[2] = { random(), random() };
                memcpy(info + 44, key, 16);
                std::lock_guard lock(g_hostedMutex);
                memcpy(g_hosted.info, info, SESSION_INFO_SIZE);
                g_hosted.flags = flags;
                g_hosted.publicSlots = publicSlots;
                g_hosted.privateSlots = privateSlots;
                g_hosted.filledPublic = 1;
                g_hosted.active = true;
                LOGF_INFO("online: hosting a game");
            }
            return true;
        }
        case 0x000B0006: // XUserSetContext: user, unused, xuid, id, value.
            if (bufferLength >= 24)
            {
                std::lock_guard lock(g_hostedMutex);
                g_contexts[words[4]] = words[5];
            }
            return false;
        case 0x000B0007: // XUserSetProperty: user, unused, xuid, id, size, data.
            if (bufferLength >= 28 && words[6] != 0 && words[5] <= 256)
            {
                const uint8_t* data = g_memory.Translate<const uint8_t>(words[6]);
                std::lock_guard lock(g_hostedMutex);
                g_properties[words[4]] = std::vector<uint8_t>(data, data + words[5]);
            }
            return false;
        case 0x000B001C: // XSessionSearchEx.
            WriteSearchResults(words);
            return true;
        case 0x000B0011: // XSessionDelete.
        {
            std::lock_guard lock(g_hostedMutex);
            g_hosted.active = false;
            if (g_openSessions.load(std::memory_order_relaxed) > 0)
                g_openSessions.fetch_sub(1, std::memory_order_relaxed);
            LOGF_INFO("online: session deleted");
            return true;
        }
        default:
            break;
        }
        // Anything else about sessions: logged in full, and accepted.
        if ((message & 0xFFFF0000) == 0x000B0000 && message != 0x000B0006 && message != 0x000B0007)
        {
            std::string text;
            for (uint32_t i = 0; words != nullptr && i < std::min<uint32_t>(bufferLength / 4, 16); i++)
                text += fmt::format(" {:08X}", words[i].get());
            LOGF_INFO("online: message {:08X}, {} bytes:{}", message, bufferLength, text);
        }
        return false;
    }
}
