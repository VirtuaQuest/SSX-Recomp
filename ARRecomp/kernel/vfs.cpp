#include <stdafx.h>
#include "vfs.h"
#include "objects.h"

namespace vfs
{
    // ---- Host file access ----

    class HostHandle
    {
    public:
        HANDLE handle = INVALID_HANDLE_VALUE;

        ~HostHandle()
        {
            if (handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(handle);
        }

        // Positional reads and writes, safe to use from several threads at once.
        size_t Read(uint64_t offset, void* buffer, size_t size) const
        {
            size_t total = 0;
            while (total < size)
            {
                OVERLAPPED overlapped{};
                uint64_t position = offset + total;
                overlapped.Offset = uint32_t(position);
                overlapped.OffsetHigh = uint32_t(position >> 32);

                DWORD chunk = DWORD(std::min<size_t>(size - total, 0x40000000));
                DWORD read = 0;
                if (!::ReadFile(handle, (uint8_t*)buffer + total, chunk, &read, &overlapped) || read == 0)
                    break;

                total += read;
            }

            return total;
        }

        size_t Write(uint64_t offset, const void* buffer, size_t size) const
        {
            OVERLAPPED overlapped{};
            overlapped.Offset = uint32_t(offset);
            overlapped.OffsetHigh = uint32_t(offset >> 32);

            DWORD written = 0;
            ::WriteFile(handle, buffer, DWORD(size), &written, &overlapped);
            return written;
        }
    };

    static int64_t ToGuestTime(const FILETIME& time)
    {
        return (int64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    static EntryInfo HostInfo(const std::filesystem::path& path)
    {
        EntryInfo info{};
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            info.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            info.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            info.writeTime = ToGuestTime(data.ftLastWriteTime);
        }

        return info;
    }

    class HostFile : public File
    {
    public:
        std::filesystem::path path;
        std::shared_ptr<HostHandle> handle;
        bool readOnly;

        EntryInfo Info() const override
        {
            auto info = HostInfo(path);
            info.readOnly = readOnly;
            return info;
        }

        size_t Read(uint64_t offset, void* buffer, size_t size) override
        {
            return handle ? handle->Read(offset, buffer, size) : 0;
        }

        size_t Write(uint64_t offset, const void* buffer, size_t size) override
        {
            return handle && !readOnly ? handle->Write(offset, buffer, size) : 0;
        }

        bool SetSize(uint64_t size) override
        {
            if (!handle || readOnly)
                return false;

            FILE_END_OF_FILE_INFO info{};
            info.EndOfFile.QuadPart = int64_t(size);
            return SetFileInformationByHandle(handle->handle, FileEndOfFileInfo, &info, sizeof(info));
        }

        void Flush() override
        {
            if (handle)
                FlushFileBuffers(handle->handle);
        }

        std::vector<DirectoryEntry> List() const override
        {
            std::vector<DirectoryEntry> entries;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(path, ec))
            {
                auto info = HostInfo(entry.path());
                info.readOnly = readOnly;
                entries.push_back({ entry.path().filename().string(), info });
            }

            return entries;
        }
    };

    // ---- Disc image (XDVDFS) ----

    constexpr uint32_t SECTOR_SIZE = 0x800;

    struct IsoEntry
    {
        uint32_t sector{};
        uint64_t size{};
        bool directory{};
        std::vector<std::string> children; // Lower case names of directory entries.
        std::string name;                  // Original case.
    };

    class IsoImage
    {
    public:
        HostHandle file;
        uint64_t partitionOffset{};
        std::unordered_map<std::string, IsoEntry> entries; // Keyed by lower case relative path, "" is the root.

        bool Open(const std::filesystem::path& path)
        {
            file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (file.handle == INVALID_HANDLE_VALUE)
                return false;

            // Game partition offsets of the disc layouts: XGD3, XGD2, XGD1 and plain XISO images.
            for (uint64_t offset : { 0x2080000ull, 0xFD90000ull, 0x18300000ull, 0ull })
            {
                char magic[20];
                if (file.Read(offset + 32 * SECTOR_SIZE, magic, sizeof(magic)) == sizeof(magic) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
                {
                    partitionOffset = offset;
                    break;
                }

                if (offset == 0)
                    return false;
            }

            uint32_t root[2];
            file.Read(partitionOffset + 32 * SECTOR_SIZE + 20, root, sizeof(root));

            IsoEntry& rootEntry = entries[""];
            rootEntry.directory = true;
            ReadDirectory("", root[0], root[1]);
            return true;
        }

        void ReadDirectory(const std::string& path, uint32_t sector, uint32_t size)
        {
            if (size == 0)
                return;

            std::vector<uint8_t> table(size);
            file.Read(partitionOffset + uint64_t(sector) * SECTOR_SIZE, table.data(), size);

            std::vector<size_t> stack{ 0 };
            std::vector<bool> visited(size / 4 + 1);
            while (!stack.empty())
            {
                size_t offset = stack.back();
                stack.pop_back();
                if (offset + 14 > size || visited[offset / 4])
                    continue;

                visited[offset / 4] = true;

                uint16_t left, right;
                uint32_t entrySector, entrySize;
                memcpy(&left, &table[offset], 2);
                memcpy(&right, &table[offset + 2], 2);
                memcpy(&entrySector, &table[offset + 4], 4);
                memcpy(&entrySize, &table[offset + 8], 4);
                uint8_t attributes = table[offset + 12];
                uint8_t nameLength = table[offset + 13];

                if (left == 0xFFFF && right == 0xFFFF)
                    continue;

                std::string name(reinterpret_cast<const char*>(&table[offset + 14]), std::min<size_t>(nameLength, size - offset - 14));
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                std::string childPath = path.empty() ? lower : path + "\\" + lower;

                IsoEntry entry;
                entry.sector = entrySector;
                entry.size = entrySize;
                entry.directory = (attributes & 0x10) != 0;
                entry.name = name;
                entries[path].children.push_back(lower);
                entries[childPath] = entry;

                if (entry.directory)
                    ReadDirectory(childPath, entrySector, entrySize);

                if (left != 0 && left != 0xFFFF)
                    stack.push_back(left * 4);
                if (right != 0 && right != 0xFFFF)
                    stack.push_back(right * 4);
            }
        }
    };

    class IsoFile : public File
    {
    public:
        IsoImage* image;
        std::string path;
        const IsoEntry* entry;

        EntryInfo Info() const override
        {
            return { entry->directory, true, entry->size, 0 };
        }

        size_t Read(uint64_t offset, void* buffer, size_t size) override
        {
            if (entry->directory || offset >= entry->size)
                return 0;

            size = size_t(std::min<uint64_t>(size, entry->size - offset));
            return image->file.Read(image->partitionOffset + uint64_t(entry->sector) * SECTOR_SIZE + offset, buffer, size);
        }

        std::vector<DirectoryEntry> List() const override
        {
            std::vector<DirectoryEntry> result;
            for (const auto& child : entry->children)
            {
                const auto& childEntry = image->entries.at(path.empty() ? child : path + "\\" + child);
                result.push_back({ childEntry.name, { childEntry.directory, true, childEntry.size, 0 } });
            }

            return result;
        }
    };

    // ---- Path resolution ----

    static std::unique_ptr<IsoImage> g_iso;
    static std::filesystem::path g_gameFolder;
    static std::filesystem::path g_userPath;
    static std::mutex g_mutex;
    static std::map<std::string, std::string> g_symbolicLinks;
    static std::map<std::string, std::filesystem::path> g_roots;

    std::filesystem::path UserPath()
    {
        return g_userPath;
    }

    std::string Normalize(std::string_view path)
    {
        std::string result;
        result.reserve(path.size());
        for (char c : path)
        {
            c = c == '/' ? '\\' : char(::tolower((unsigned char)c));
            if (c == '\\' && !result.empty() && result.back() == '\\')
                continue;

            result += c;
        }

        if (result.rfind("\\??\\", 0) == 0)
            result.erase(0, 4);

        while (result.size() > 1 && result.back() == '\\')
            result.pop_back();

        return result;
    }

    struct Resolved
    {
        enum class Kind { Game, Host, Invalid } kind = Kind::Invalid;
        std::string relative;                // For the game device: lower case path inside the disc.
        std::filesystem::path hostPath;      // For host devices.
    };

    static bool IsGameRoot(std::string_view root)
    {
        return root == "game:" || root == "d:" || root == "\\device\\cdrom0" || root == "dvd:";
    }

    static Resolved Resolve(std::string_view guestPath)
    {
        std::string path = Normalize(guestPath);

        std::lock_guard lock(g_mutex);

        // Symbolic links, longest match first. Links may point at other links.
        for (int depth = 0; depth < 8; depth++)
        {
            bool replaced = false;
            for (auto it = g_symbolicLinks.rbegin(); it != g_symbolicLinks.rend(); ++it)
            {
                const auto& [link, target] = *it;
                if (path.rfind(link, 0) == 0 && (path.size() == link.size() || path[link.size()] == '\\'))
                {
                    path = target + path.substr(link.size());
                    replaced = true;
                    break;
                }
            }

            if (!replaced)
                break;
        }

        // Split into the device ("x:" or "\device\name") and the relative path.
        std::string root, relative;
        if (path.rfind("\\device\\", 0) == 0)
        {
            size_t end = path.find('\\', 8);
            root = path.substr(0, end);
            relative = end == std::string::npos ? "" : path.substr(end + 1);

            // Hard disk partitions: \device\harddisk0\partitionN.
            if (root == "\\device\\harddisk0" && relative.rfind("partition", 0) == 0)
            {
                size_t next = relative.find('\\');
                root += "\\" + relative.substr(0, next);
                relative = next == std::string::npos ? "" : relative.substr(next + 1);
            }
        }
        else
        {
            size_t colon = path.find(':');
            if (colon == std::string::npos)
                return {};

            root = path.substr(0, colon + 1);
            relative = path.size() > colon + 2 ? path.substr(colon + 2) : "";
        }

        // Reject anything that could escape a host folder.
        for (size_t start = 0; start <= relative.size();)
        {
            size_t end = relative.find('\\', start);
            std::string_view part(relative.data() + start, (end == std::string::npos ? relative.size() : end) - start);
            if (part == "..")
                return {};

            if (end == std::string::npos)
                break;
            start = end + 1;
        }

        Resolved resolved;
        if (IsGameRoot(root))
        {
            if (g_iso)
            {
                resolved.kind = Resolved::Kind::Game;
                resolved.relative = relative;
            }
            else
            {
                resolved.kind = Resolved::Kind::Host;
                resolved.hostPath = g_gameFolder / relative;
            }

            return resolved;
        }

        auto mapped = g_roots.find(root);
        std::filesystem::path base;
        if (mapped != g_roots.end())
        {
            base = mapped->second;
        }
        else
        {
            std::string folder = root;
            std::replace(folder.begin(), folder.end(), '\\', '_');
            std::replace(folder.begin(), folder.end(), ':', '_');
            base = g_userPath / "devices" / folder;
        }

        resolved.kind = Resolved::Kind::Host;
        resolved.hostPath = relative.empty() ? base : base / relative;
        return resolved;
    }

    // ---- Public interface ----

    bool Mount(const std::filesystem::path& gamePath, const std::filesystem::path& userPath)
    {
        g_userPath = std::filesystem::absolute(userPath);
        std::error_code ec;
        std::filesystem::create_directories(g_userPath, ec);

        if (std::filesystem::is_directory(gamePath))
        {
            g_gameFolder = gamePath;
            LOGF_INFO("game files from folder {}", gamePath.string());
            return true;
        }

        auto iso = std::make_unique<IsoImage>();
        if (!iso->Open(gamePath))
        {
            LOGF_ERROR("{} is not an Xbox 360 disc image", gamePath.string());
            return false;
        }

        LOGF_INFO("game files from disc image {} ({} entries)", gamePath.string(), iso->entries.size());
        g_iso = std::move(iso);
        return true;
    }

    void MapRoot(std::string_view root, const std::filesystem::path& hostPath)
    {
        std::error_code ec;
        std::filesystem::create_directories(hostPath, ec);

        std::lock_guard lock(g_mutex);
        g_roots[Normalize(root)] = hostPath;
    }

    void UnmapRoot(std::string_view root)
    {
        std::lock_guard lock(g_mutex);
        g_roots.erase(Normalize(root));
    }

    void CreateSymbolicLink(std::string_view link, std::string_view target)
    {
        std::lock_guard lock(g_mutex);
        g_symbolicLinks[Normalize(link)] = Normalize(target);
    }

    void DeleteSymbolicLink(std::string_view link)
    {
        std::lock_guard lock(g_mutex);
        g_symbolicLinks.erase(Normalize(link));
    }

    std::unique_ptr<File> Open(std::string_view path, Disposition disposition, bool write, bool directory, OpenResult& result)
    {
        auto resolved = Resolve(path);

        if (resolved.kind == Resolved::Kind::Invalid)
        {
            result = OpenResult::PathNotFound;
            return nullptr;
        }

        if (resolved.kind == Resolved::Kind::Game)
        {
            auto it = g_iso->entries.find(resolved.relative);
            if (it == g_iso->entries.end())
            {
                result = disposition == Disposition::Open || disposition == Disposition::Overwrite ? OpenResult::NotFound : OpenResult::AccessDenied;
                return nullptr;
            }

            if (write || disposition == Disposition::Create || disposition == Disposition::Supersede ||
                disposition == Disposition::Overwrite || disposition == Disposition::OverwriteIf)
            {
                result = disposition == Disposition::Create ? OpenResult::Exists : OpenResult::AccessDenied;
                return nullptr;
            }

            auto file = std::make_unique<IsoFile>();
            file->image = g_iso.get();
            file->path = resolved.relative;
            file->entry = &it->second;
            result = OpenResult::Opened;
            return file;
        }

        const auto& hostPath = resolved.hostPath;
        bool readOnly = !g_gameFolder.empty() && hostPath.native().rfind(g_gameFolder.native(), 0) == 0;
        std::error_code ec;
        bool exists = std::filesystem::exists(hostPath, ec);
        bool isDirectory = exists && std::filesystem::is_directory(hostPath, ec);

        if (exists && disposition == Disposition::Create)
        {
            result = OpenResult::Exists;
            return nullptr;
        }

        if (!exists && (disposition == Disposition::Open || disposition == Disposition::Overwrite))
        {
            result = std::filesystem::exists(hostPath.parent_path(), ec) ? OpenResult::NotFound : OpenResult::PathNotFound;
            return nullptr;
        }

        if (!exists && !std::filesystem::exists(hostPath.parent_path(), ec))
        {
            result = OpenResult::PathNotFound;
            return nullptr;
        }

        auto file = std::make_unique<HostFile>();
        file->path = hostPath;
        file->readOnly = readOnly;

        if (directory || isDirectory)
        {
            if (!exists)
            {
                if (readOnly)
                {
                    result = OpenResult::AccessDenied;
                    return nullptr;
                }

                std::filesystem::create_directory(hostPath, ec);
            }
            else if (!isDirectory)
            {
                result = OpenResult::NotDirectory;
                return nullptr;
            }

            result = exists ? OpenResult::Opened : OpenResult::Created;
            return file;
        }

        bool truncate = exists && (disposition == Disposition::Supersede || disposition == Disposition::Overwrite || disposition == Disposition::OverwriteIf);
        if ((truncate || !exists) && readOnly)
        {
            result = OpenResult::AccessDenied;
            return nullptr;
        }

        DWORD creation = !exists ? CREATE_NEW : (truncate ? TRUNCATE_EXISTING : OPEN_EXISTING);
        DWORD access = GENERIC_READ | ((write || truncate || !exists) && !readOnly ? GENERIC_WRITE : 0);
        file->handle = std::make_shared<HostHandle>();
        file->handle->handle = CreateFileW(hostPath.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file->handle->handle == INVALID_HANDLE_VALUE)
        {
            LOGF_WARNING("failed to open {} (error {})", hostPath.string(), GetLastError());
            result = OpenResult::AccessDenied;
            return nullptr;
        }

        file->readOnly = readOnly || !(access & GENERIC_WRITE);
        result = !exists ? OpenResult::Created : (truncate ? OpenResult::Overwritten : OpenResult::Opened);
        return file;
    }

    std::optional<EntryInfo> Stat(std::string_view path)
    {
        auto resolved = Resolve(path);
        if (resolved.kind == Resolved::Kind::Game)
        {
            auto it = g_iso->entries.find(resolved.relative);
            if (it == g_iso->entries.end())
                return std::nullopt;

            return EntryInfo{ it->second.directory, true, it->second.size, 0 };
        }

        if (resolved.kind == Resolved::Kind::Host)
        {
            std::error_code ec;
            if (!std::filesystem::exists(resolved.hostPath, ec))
                return std::nullopt;

            return HostInfo(resolved.hostPath);
        }

        return std::nullopt;
    }

    bool Delete(std::string_view path)
    {
        auto resolved = Resolve(path);
        if (resolved.kind != Resolved::Kind::Host)
            return false;

        std::error_code ec;
        return std::filesystem::remove(resolved.hostPath, ec);
    }

    bool Rename(std::string_view from, std::string_view to)
    {
        auto source = Resolve(from);
        auto destination = Resolve(to);
        if (source.kind != Resolved::Kind::Host || destination.kind != Resolved::Kind::Host)
            return false;

        std::error_code ec;
        std::filesystem::rename(source.hostPath, destination.hostPath, ec);
        return !ec;
    }

    bool ExtractFile(std::string_view guestPath, const std::filesystem::path& hostPath)
    {
        OpenResult result;
        auto file = Open(guestPath, Disposition::Open, false, false, result);
        if (!file)
            return false;

        std::vector<uint8_t> data(file->Info().size);
        if (file->Read(0, data.data(), data.size()) != data.size())
            return false;

        FILE* out = _wfopen(hostPath.c_str(), L"wb");
        if (out == nullptr)
            return false;

        fwrite(data.data(), 1, data.size(), out);
        fclose(out);
        return true;
    }
}
