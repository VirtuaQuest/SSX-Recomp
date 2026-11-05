#include "package.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

namespace package
{
    namespace
    {
        struct File
        {
            FILE* handle{};
            explicit File(const std::filesystem::path& path) { _wfopen_s(&handle, path.c_str(), L"rb"); }
            ~File() { if (handle) fclose(handle); }
            File(const File&) = delete;
            File& operator=(const File&) = delete;

            bool Read(uint64_t offset, void* buffer, size_t size)
            {
                return handle && _fseeki64(handle, int64_t(offset), SEEK_SET) == 0 && fread(buffer, 1, size, handle) == size;
            }
        };

        uint32_t Big32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint32_t Little24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

        constexpr uint32_t SECTOR = 0x800;

        // A name in the root directory of a disc image, whose directories are binary trees of entries.
        bool FindInIso(File& file, const char* name, std::vector<uint8_t>& out, std::wstring& error)
        {
            uint64_t partition = 0;
            bool found = false;
            for (uint64_t offset : { 0x2080000ull, 0xFD90000ull, 0x18300000ull, 0ull }) // XGD3, XGD2, XGD1, plain XISO.
            {
                char magic[20];
                if (file.Read(offset + 32 * SECTOR, magic, sizeof(magic)) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
                {
                    partition = offset;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                error = L"This isn't an Xbox 360 disc image.";
                return false;
            }

            uint32_t root[2];
            if (!file.Read(partition + 32 * SECTOR + 20, root, sizeof(root)) || root[1] == 0 || root[1] > 64 * 1024 * 1024)
            {
                error = L"The disc image's directory can't be read.";
                return false;
            }

            std::vector<uint8_t> table(root[1]);
            if (!file.Read(partition + uint64_t(root[0]) * SECTOR, table.data(), table.size()))
            {
                error = L"The disc image's directory can't be read.";
                return false;
            }

            std::vector<size_t> stack{ 0 };
            std::vector<bool> visited(table.size() / 4 + 1);
            while (!stack.empty())
            {
                size_t at = stack.back();
                stack.pop_back();
                if (at + 14 > table.size() || visited[at / 4])
                    continue;
                visited[at / 4] = true;

                uint16_t left, right;
                uint32_t sector, size;
                memcpy(&left, &table[at], 2);
                memcpy(&right, &table[at + 2], 2);
                memcpy(&sector, &table[at + 4], 4);
                memcpy(&size, &table[at + 8], 4);
                if (left == 0xFFFF && right == 0xFFFF)
                    continue;

                size_t length = std::min<size_t>(table[at + 13], table.size() - at - 14);
                std::string entry(reinterpret_cast<const char*>(&table[at + 14]), length);
                if (_stricmp(entry.c_str(), name) == 0)
                {
                    out.resize(size);
                    if (!file.Read(partition + uint64_t(sector) * SECTOR, out.data(), size))
                    {
                        error = L"The disc image is shorter than it says. It may be damaged.";
                        return false;
                    }
                    return true;
                }
                if (left != 0 && left != 0xFFFF)
                    stack.push_back(size_t(left) * 4);
                if (right != 0 && right != 0xFFFF)
                    stack.push_back(size_t(right) * 4);
            }

            error = L"This disc image has no game executable.";
            return false;
        }

        // STFS: data blocks of 4 KB with tables of their hashes in between, one per 170 blocks, then one per 170 of those, and so on.
        constexpr uint32_t BLOCK = 0x1000, SPACING = 170, SPACING2 = SPACING * SPACING;

        struct Stfs
        {
            File file;
            uint64_t first{};
            uint32_t shift{}; // Read only packages have one copy of each table, others two.
            uint32_t tableBlocks{}, tableStart{};

            explicit Stfs(const std::filesystem::path& path) : file(path) {}

            bool Open()
            {
                uint8_t header[0x400];
                if (!file.Read(0, header, sizeof(header)))
                    return false;
                if (memcmp(header, "LIVE", 4) != 0 && memcmp(header, "PIRS", 4) != 0 && memcmp(header, "CON ", 4) != 0)
                    return false;
                if (header[0x379] != 0x24) // The size of the descriptor of the STFS file system.
                    return false;
                first = (uint64_t(Big32(header + 0x340)) + 0xFFF) & ~uint64_t(0xFFF);
                shift = (header[0x37B] & 1) ? 0 : 1;
                tableBlocks = uint32_t(header[0x37C]) | uint32_t(header[0x37D]) << 8;
                tableStart = Little24(header + 0x37E);
                return true;
            }

            uint64_t DataOffset(uint32_t block) const
            {
                uint64_t index = block, base = SPACING;
                for (int i = 0; i < 3; i++)
                {
                    index += ((uint64_t(block) + base) / base) << shift;
                    if (block < base)
                        break;
                    base *= SPACING;
                }
                return first + (index << 12);
            }

            // The table of hashes covering a block.
            uint64_t TableOffset(uint32_t block) const
            {
                uint64_t index = 0;
                if (block >= SPACING)
                {
                    index = uint64_t(block / SPACING) * (SPACING + (uint64_t(1) << shift));
                    index += uint64_t(block / SPACING2 + 1) << shift;
                    if (block / SPACING2 != 0)
                        index += uint64_t(1) << shift;
                }
                return first + (index << 12);
            }

            // A block, checked against its hash, and the number of the next block.
            bool ReadBlock(uint32_t block, uint8_t* data, uint32_t& next)
            {
                uint8_t entry[24];
                if (!file.Read(DataOffset(block), data, BLOCK) || !file.Read(TableOffset(block) + uint64_t(block % SPACING) * 24, entry, sizeof(entry)))
                    return false;
                std::string actual;
                uint8_t digest[20];
                if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, data, BLOCK, digest, sizeof(digest)) != 0 || memcmp(digest, entry, 20) != 0)
                    return false;
                next = uint32_t(entry[21]) << 16 | uint32_t(entry[22]) << 8 | entry[23];
                return true;
            }
        };

        // What a package says its files are called comes from outside: nothing may lead out of the folder.
        bool SafeName(const std::string& name)
        {
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string::npos)
                return false;
            return std::all_of(name.begin(), name.end(), [](char c) { return uint8_t(c) >= 0x20; }) && name.back() != '.' && name.back() != ' ';
        }
    }

    std::string Sha1(const void* data, size_t size)
    {
        uint8_t digest[20];
        if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), digest, sizeof(digest)) != 0)
            return {};
        std::string text;
        for (uint8_t byte : digest)
        {
            char pair[3];
            sprintf_s(pair, "%02x", byte);
            text += pair;
        }
        return text;
    }

    bool ReadGameFile(const std::filesystem::path& game, const char* name, std::vector<uint8_t>& out, std::wstring& error)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(game, ec))
        {
            std::ifstream stream(game / name, std::ios::binary);
            if (!stream)
            {
                error = L"This folder has no game executable (default.xex).";
                return false;
            }
            out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return true;
        }

        File file(game);
        if (!file.handle)
        {
            error = L"This file can't be opened.";
            return false;
        }
        return FindInIso(file, name, out, error);
    }

    Info ReadInfo(const std::filesystem::path& path)
    {
        Info info;
        File file(path);
        uint8_t header[0x500];
        if (!file.Read(0, header, sizeof(header)))
            return info;
        if (memcmp(header, "LIVE", 4) != 0 && memcmp(header, "PIRS", 4) != 0 && memcmp(header, "CON ", 4) != 0)
            return info;
        info.valid = true;
        info.contentType = Big32(header + 0x344);
        info.titleId = Big32(header + 0x360);
        for (size_t i = 0; i < 64; i++)
        {
            wchar_t c = wchar_t(uint16_t(header[0x411 + i * 2]) << 8 | header[0x411 + i * 2 + 1]);
            if (c == 0)
                break;
            info.name += c;
        }
        return info;
    }

    bool Extract(const std::filesystem::path& path, const std::filesystem::path& folder, std::wstring& error)
    {
        Stfs stfs(path);
        if (!stfs.Open())
        {
            error = L"not a package this can read";
            return false;
        }

        struct Entry
        {
            std::string name;
            bool directory, contiguous;
            uint32_t start, size;
            int parent;
        };

        std::vector<Entry> entries;
        uint8_t block[BLOCK];
        uint32_t tableBlock = stfs.tableStart;
        for (uint32_t i = 0; i < stfs.tableBlocks; i++)
        {
            uint32_t next = 0;
            if (!stfs.ReadBlock(tableBlock, block, next))
            {
                error = L"its list of files is damaged";
                return false;
            }
            for (uint32_t at = 0; at < BLOCK; at += 0x40)
            {
                const uint8_t* e = block + at;
                if (e[0] == 0)
                    continue;
                uint8_t flags = e[0x28];
                std::string name(reinterpret_cast<const char*>(e), std::min<size_t>(flags & 0x3F, 40));
                int16_t parent = int16_t(uint16_t(e[0x32]) << 8 | e[0x33]);
                entries.push_back({ name, (flags & 0x80) != 0, (flags & 0x40) != 0, Little24(e + 0x2F), Big32(e + 0x34), parent });
            }
            tableBlock = next;
        }

        std::map<int, std::filesystem::path> paths;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::vector<uint8_t> data;
        for (size_t i = 0; i < entries.size(); i++)
        {
            const Entry& entry = entries[i];
            if (!SafeName(entry.name))
            {
                error = L"it has a file with an unsafe name";
                return false;
            }
            std::filesystem::path relative = std::filesystem::u8path(entry.name);
            if (entry.parent >= 0)
            {
                auto parent = paths.find(entry.parent);
                if (parent == paths.end())
                {
                    error = L"its list of files is damaged";
                    return false;
                }
                relative = parent->second / relative;
            }
            paths[int(i)] = relative;
            std::filesystem::path target = folder / relative;
            if (entry.directory)
            {
                std::filesystem::create_directories(target, ec);
                continue;
            }

            std::filesystem::create_directories(target.parent_path(), ec);
            std::ofstream output(target, std::ios::binary);
            uint32_t current = entry.start, left = entry.size;
            while (left > 0)
            {
                uint32_t next = 0;
                if (!stfs.ReadBlock(current, block, next))
                {
                    error = L"it is damaged: a block doesn't match its hash";
                    return false;
                }
                uint32_t size = std::min(left, BLOCK);
                output.write(reinterpret_cast<const char*>(block), size);
                left -= size;
                current = entry.contiguous ? current + 1 : next;
            }
            if (!output.good())
            {
                error = L"its files can't be written (is the disk full?)";
                return false;
            }
        }
        return true;
    }
}
