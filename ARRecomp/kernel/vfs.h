#pragma once

// Virtual file system for guest paths.
//
// The game disc ("game:", "d:", "\Device\Cdrom0") is read straight from the .iso, or from a folder with the
// extracted files. Every other device ("cache:", "\Device\Harddisk0\...", content roots such as save games) maps
// to a writable folder inside the user directory.
namespace vfs
{
    struct EntryInfo
    {
        bool directory{};
        bool readOnly{};
        uint64_t size{};
        int64_t writeTime{}; // Guest FILETIME.
    };

    struct DirectoryEntry
    {
        std::string name;
        EntryInfo info;
    };

    class File
    {
    public:
        virtual ~File() = default;

        virtual EntryInfo Info() const = 0;
        virtual size_t Read(uint64_t offset, void* buffer, size_t size) = 0;
        virtual size_t Write(uint64_t offset, const void* buffer, size_t size) { return 0; }
        virtual bool SetSize(uint64_t size) { return false; }
        virtual void Flush() {}
        virtual std::vector<DirectoryEntry> List() const { return {}; }
    };

    enum class Disposition
    {
        Supersede,   // Replace if it exists, create otherwise.
        Open,        // Fail if it does not exist.
        Create,      // Fail if it exists.
        OpenIf,      // Open or create.
        Overwrite,   // Truncate, fail if it does not exist.
        OverwriteIf, // Truncate or create.
    };

    enum class OpenResult
    {
        Opened,
        Created,
        Overwritten,
        NotFound,
        PathNotFound,
        Exists,
        AccessDenied,
        IsDirectory,
        NotDirectory,
    };

    bool Mount(const std::filesystem::path& gamePath, const std::filesystem::path& userPath);

    // Maps a device or root name (for example "save:") to a host folder.
    void MapRoot(std::string_view root, const std::filesystem::path& hostPath);
    void UnmapRoot(std::string_view root);

    // Symbolic links created by the game with ObCreateSymbolicLink.
    void CreateSymbolicLink(std::string_view link, std::string_view target);
    void DeleteSymbolicLink(std::string_view link);

    std::unique_ptr<File> Open(std::string_view path, Disposition disposition, bool write, bool directory, OpenResult& result);
    std::optional<EntryInfo> Stat(std::string_view path);
    bool Delete(std::string_view path);
    bool Rename(std::string_view from, std::string_view to);

    bool ExtractFile(std::string_view guestPath, const std::filesystem::path& hostPath);

    // Normalized form of a guest path, as used for lookups. Exposed for logging.
    std::string Normalize(std::string_view path);

    std::filesystem::path UserPath();
}
