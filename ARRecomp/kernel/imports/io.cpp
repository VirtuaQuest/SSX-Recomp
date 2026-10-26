// File system: Nt file functions, symbolic links and the I/O manager.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/vfs.h>

struct FileObject : KernelObject
{
    std::unique_ptr<vfs::File> file;
    std::string path;
    uint64_t position{};
    bool directory{};

    // Directory enumeration state for NtQueryDirectoryFile.
    std::vector<vfs::DirectoryEntry> entries;
    size_t nextEntry{};
    bool listed{};

    FileObject() : KernelObject(ObjectType::File) {}
};

constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x01;
constexpr uint32_t X_FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t X_FILE_ATTRIBUTE_NORMAL = 0x80;

constexpr uint32_t X_FILE_SUPERSEDED = 0;
constexpr uint32_t X_FILE_OPENED = 1;
constexpr uint32_t X_FILE_CREATED = 2;
constexpr uint32_t X_FILE_OVERWRITTEN = 3;

static uint32_t Attributes(const vfs::EntryInfo& info)
{
    uint32_t attributes = info.directory ? X_FILE_ATTRIBUTE_DIRECTORY : 0;
    if (info.readOnly)
        attributes |= X_FILE_ATTRIBUTE_READONLY;

    return attributes != 0 ? attributes : X_FILE_ATTRIBUTE_NORMAL;
}

static uint64_t AllocationSize(uint64_t size)
{
    return (size + 0xFFF) & ~0xFFFull;
}

// Resolves OBJECT_ATTRIBUTES to a guest path, including paths relative to an open directory.
static std::string ObjectPath(const XOBJECT_ATTRIBUTES* attributes)
{
    std::string name = ReadAnsiString(attributes->Name.get());
    if (uint32_t root = attributes->RootDirectory; root != 0)
    {
        if (auto directory = LookupHandle<FileObject>(root, ObjectType::File))
            return directory->path + "\\" + name;
    }

    return name;
}

static void SetIoStatus(XIO_STATUS_BLOCK* ioStatus, uint32_t status, uint32_t information)
{
    if (ioStatus != nullptr)
    {
        ioStatus->Status = status;
        ioStatus->Information = information;
    }
}

static void SignalCompletion(uint32_t event, uint32_t apcRoutine)
{
    if (event != 0)
    {
        if (auto object = LookupHandle(event); object && object->guestObject != 0)
            SetDispatcherEvent(object->Header());
    }

    if (apcRoutine != 0)
        LOGF_WARNING("I/O completion APC 0x{:08X} is not called", apcRoutine);
}

// Simple wildcard match for directory queries: '*' and '?', case insensitive.
static bool MatchPattern(std::string_view pattern, std::string_view name)
{
    if (pattern.empty())
        return name.empty();

    if (pattern[0] == '*')
    {
        for (size_t i = 0; i <= name.size(); i++)
        {
            if (MatchPattern(pattern.substr(1), name.substr(i)))
                return true;
        }

        return false;
    }

    if (name.empty())
        return false;

    if (pattern[0] != '?' && tolower((unsigned char)pattern[0]) != tolower((unsigned char)name[0]))
        return false;

    return MatchPattern(pattern.substr(1), name.substr(1));
}

namespace imports
{
    static uint32_t OpenFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
        XIO_STATUS_BLOCK* ioStatus, uint32_t disposition, uint32_t options)
    {
        std::string path = ObjectPath(attributes);

        // GENERIC_WRITE, GENERIC_ALL, FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_ATTRIBUTES, DELETE
        bool write = (desiredAccess & (0x40000000 | 0x10000000 | 0x2 | 0x4 | 0x100 | 0x10000)) != 0;
        bool directory = (options & 0x1) != 0; // FILE_DIRECTORY_FILE

        static const vfs::Disposition dispositions[] = {
            vfs::Disposition::Supersede, vfs::Disposition::Open, vfs::Disposition::Create,
            vfs::Disposition::OpenIf, vfs::Disposition::Overwrite, vfs::Disposition::OverwriteIf,
        };

        vfs::OpenResult result;
        auto file = vfs::Open(path, dispositions[std::min<uint32_t>(disposition, 5)], write, directory, result);

        uint32_t status = X_STATUS_SUCCESS;
        uint32_t information = X_FILE_OPENED;
        switch (result)
        {
        case vfs::OpenResult::Opened: information = X_FILE_OPENED; break;
        case vfs::OpenResult::Created: information = X_FILE_CREATED; break;
        case vfs::OpenResult::Overwritten: information = disposition == 0 ? X_FILE_SUPERSEDED : X_FILE_OVERWRITTEN; break;
        case vfs::OpenResult::NotFound: status = X_STATUS_OBJECT_NAME_NOT_FOUND; break;
        case vfs::OpenResult::PathNotFound: status = X_STATUS_OBJECT_PATH_NOT_FOUND; break;
        case vfs::OpenResult::Exists: status = X_STATUS_OBJECT_NAME_COLLISION; break;
        case vfs::OpenResult::AccessDenied: status = X_STATUS_ACCESS_DENIED; break;
        case vfs::OpenResult::IsDirectory: status = X_STATUS_FILE_IS_A_DIRECTORY; break;
        case vfs::OpenResult::NotDirectory: status = X_STATUS_NOT_A_DIRECTORY; break;
        }

        if (file && (options & 0x40) && file->Info().directory) // FILE_NON_DIRECTORY_FILE
        {
            file.reset();
            status = X_STATUS_FILE_IS_A_DIRECTORY;
        }

        LOGF_INFO("{} {} -> 0x{:08X}", write ? "write" : "read", path, status);

        SetIoStatus(ioStatus, status, status == X_STATUS_SUCCESS ? information : 0);
        if (status != X_STATUS_SUCCESS)
            return status;

        auto object = std::make_shared<FileObject>();
        object->directory = file->Info().directory;
        object->file = std::move(file);
        object->path = path;
        *handle = CreateHandle(object);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtCreateFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
        XIO_STATUS_BLOCK* ioStatus, be<uint64_t>* allocationSize, uint32_t fileAttributes, uint32_t shareAccess,
        uint32_t disposition, uint32_t options)
    {
        return OpenFile(handle, desiredAccess, attributes, ioStatus, disposition, options);
    }

    uint32_t NtOpenFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
        XIO_STATUS_BLOCK* ioStatus, uint32_t options)
    {
        return OpenFile(handle, desiredAccess, attributes, ioStatus, 1, options);
    }

    // A byte offset of -2 (FILE_USE_FILE_POINTER_POSITION) or no offset uses the current position.
    static uint64_t StartOffset(const FileObject& file, const be<int64_t>* byteOffset)
    {
        if (byteOffset == nullptr || byteOffset->get() == -2)
            return file.position;

        return uint64_t(byteOffset->get());
    }

    uint32_t NtReadFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
        uint8_t* buffer, uint32_t length, be<int64_t>* byteOffset)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint64_t offset = StartOffset(*file, byteOffset);
        uint64_t size = file->file->Info().size;
        uint32_t status = X_STATUS_SUCCESS;
        size_t read = 0;

        if (offset >= size && length != 0)
            status = X_STATUS_END_OF_FILE;
        else
            read = file->file->Read(offset, buffer, length);

        file->position = offset + read;
        SetIoStatus(ioStatus, status, uint32_t(read));
        SignalCompletion(event, apcRoutine);
        return status;
    }

    // FILE_SEGMENT_ELEMENT array: one guest page address (in a 64 bit slot) per 4 KB of the transfer.
    uint32_t NtReadFileScatter(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
        be<uint64_t>* segments, uint32_t length, be<int64_t>* byteOffset)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint64_t offset = StartOffset(*file, byteOffset);
        size_t total = 0;
        for (uint32_t i = 0; total < length; i++)
        {
            size_t chunk = std::min<size_t>(0x1000, length - total);
            size_t read = file->file->Read(offset + total, g_memory.Translate(uint32_t(segments[i].get())), chunk);
            total += read;
            if (read < chunk)
                break;
        }

        file->position = offset + total;
        uint32_t status = total == 0 && length != 0 ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
        SetIoStatus(ioStatus, status, uint32_t(total));
        SignalCompletion(event, apcRoutine);
        return status;
    }

    uint32_t NtWriteFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
        const uint8_t* buffer, uint32_t length, be<int64_t>* byteOffset)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint64_t offset = StartOffset(*file, byteOffset);
        size_t written = file->file->Write(offset, buffer, length);
        file->position = offset + written;

        uint32_t status = written == length ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
        SetIoStatus(ioStatus, status, uint32_t(written));
        SignalCompletion(event, apcRoutine);
        return status;
    }

    uint32_t NtWriteFileGather(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
        be<uint64_t>* segments, uint32_t length, be<int64_t>* byteOffset)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint64_t offset = StartOffset(*file, byteOffset);
        size_t total = 0;
        for (uint32_t i = 0; total < length; i++)
        {
            size_t chunk = std::min<size_t>(0x1000, length - total);
            size_t written = file->file->Write(offset + total, g_memory.Translate(uint32_t(segments[i].get())), chunk);
            total += written;
            if (written < chunk)
                break;
        }

        file->position = offset + total;
        uint32_t status = total == length ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
        SetIoStatus(ioStatus, status, uint32_t(total));
        SignalCompletion(event, apcRoutine);
        return status;
    }

    uint32_t NtFlushBuffersFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        file->file->Flush();
        SetIoStatus(ioStatus, X_STATUS_SUCCESS, 0);
        return X_STATUS_SUCCESS;
    }

    // FILE_NETWORK_OPEN_INFORMATION: creation, last access, last write and change times, allocation size,
    // end of file, attributes.
    static void WriteNetworkOpenInformation(uint8_t* out, const vfs::EntryInfo& info)
    {
        auto* fields = reinterpret_cast<be<uint64_t>*>(out);
        for (int i = 0; i < 4; i++)
            fields[i] = uint64_t(info.writeTime);

        fields[4] = AllocationSize(info.size);
        fields[5] = info.size;
        *reinterpret_cast<be<uint32_t>*>(out + 0x30) = Attributes(info);
        *reinterpret_cast<be<uint32_t>*>(out + 0x34) = 0;
    }

    uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* attributes, uint8_t* information)
    {
        std::string path = ObjectPath(attributes);
        auto info = vfs::Stat(path);
        if (!info.has_value())
        {
            LOGF_TRACE("{} not found", path);
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }

        WriteNetworkOpenInformation(information, *info);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtQueryInformationFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus, uint8_t* information, uint32_t length, uint32_t infoClass)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        auto info = file->file->Info();
        uint32_t written = 0;

        switch (infoClass)
        {
        case 4: // FileBasicInformation: four times, attributes.
        {
            auto* times = reinterpret_cast<be<uint64_t>*>(information);
            for (int i = 0; i < 4; i++)
                times[i] = uint64_t(info.writeTime);
            *reinterpret_cast<be<uint32_t>*>(information + 0x20) = Attributes(info);
            *reinterpret_cast<be<uint32_t>*>(information + 0x24) = 0;
            written = 0x28;
            break;
        }

        case 5: // FileStandardInformation: allocation size, end of file, links, delete pending, directory.
            *reinterpret_cast<be<uint64_t>*>(information + 0x00) = AllocationSize(info.size);
            *reinterpret_cast<be<uint64_t>*>(information + 0x08) = info.size;
            *reinterpret_cast<be<uint32_t>*>(information + 0x10) = 1;
            information[0x14] = 0;
            information[0x15] = info.directory ? 1 : 0;
            written = 0x18;
            break;

        case 6: // FileInternalInformation: index number.
            *reinterpret_cast<be<uint64_t>*>(information) = std::hash<std::string>{}(file->path);
            written = 8;
            break;

        case 14: // FilePositionInformation
            *reinterpret_cast<be<uint64_t>*>(information) = file->position;
            written = 8;
            break;

        case 20: // FileEndOfFileInformation
            *reinterpret_cast<be<uint64_t>*>(information) = info.size;
            written = 8;
            break;

        case 34: // FileNetworkOpenInformation
            WriteNetworkOpenInformation(information, info);
            written = 0x38;
            break;

        default:
            LOGF_WARNING("unsupported information class {} for {}", infoClass, file->path);
            SetIoStatus(ioStatus, X_STATUS_INVALID_INFO_CLASS, 0);
            return X_STATUS_INVALID_INFO_CLASS;
        }

        SetIoStatus(ioStatus, X_STATUS_SUCCESS, written);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtSetInformationFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus, uint8_t* information, uint32_t length, uint32_t infoClass)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint32_t status = X_STATUS_SUCCESS;
        switch (infoClass)
        {
        case 4: // FileBasicInformation: times and attributes are not stored.
            break;

        case 13: // FileDispositionInformation: delete on close.
            if (information[0] != 0 && !vfs::Delete(file->path))
                status = X_STATUS_ACCESS_DENIED;
            break;

        case 14: // FilePositionInformation
            file->position = *reinterpret_cast<be<uint64_t>*>(information);
            break;

        case 19: // FileAllocationInformation
        case 20: // FileEndOfFileInformation
            if (!file->file->SetSize(*reinterpret_cast<be<uint64_t>*>(information)))
                status = X_STATUS_ACCESS_DENIED;
            break;

        case 10: // FileRenameInformation: ReplaceIfExists, RootDirectory, ANSI_STRING FileName.
        {
            auto* name = reinterpret_cast<XANSI_STRING*>(information + 8);
            if (!vfs::Rename(file->path, ReadAnsiString(name)))
                status = X_STATUS_ACCESS_DENIED;
            break;
        }

        default:
            LOGF_WARNING("unsupported information class {} for {}", infoClass, file->path);
            status = X_STATUS_INVALID_INFO_CLASS;
            break;
        }

        SetIoStatus(ioStatus, status, 0);
        return status;
    }

    uint32_t NtQueryVolumeInformationFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus, uint8_t* information, uint32_t length, uint32_t infoClass)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint32_t written = 0;
        switch (infoClass)
        {
        case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label.
            memset(information, 0, std::min<uint32_t>(length, 0x18));
            *reinterpret_cast<be<uint32_t>*>(information + 0x08) = 0x12345678;
            written = 0x18;
            break;

        case 3: // FileFsSizeInformation: total and available allocation units, sectors per unit, bytes per sector.
            *reinterpret_cast<be<uint64_t>*>(information + 0x00) = 0x400000;
            *reinterpret_cast<be<uint64_t>*>(information + 0x08) = 0x300000;
            *reinterpret_cast<be<uint32_t>*>(information + 0x10) = 8;
            *reinterpret_cast<be<uint32_t>*>(information + 0x14) = 0x200;
            written = 0x18;
            break;

        case 5: // FileFsAttributeInformation: attributes, maximum name length, name length, name.
        {
            const char* name = "FATX";
            *reinterpret_cast<be<uint32_t>*>(information + 0x00) = 0;
            *reinterpret_cast<be<uint32_t>*>(information + 0x04) = 255;
            *reinterpret_cast<be<uint32_t>*>(information + 0x08) = uint32_t(strlen(name) * 2);
            for (size_t i = 0; name[i] != '\0' && 0x0C + i * 2 + 2 <= length; i++)
                *reinterpret_cast<be<uint16_t>*>(information + 0x0C + i * 2) = uint16_t(name[i]);
            written = 0x0C + uint32_t(strlen(name) * 2);
            break;
        }

        default:
            LOGF_WARNING("unsupported volume information class {}", infoClass);
            SetIoStatus(ioStatus, X_STATUS_INVALID_INFO_CLASS, 0);
            return X_STATUS_INVALID_INFO_CLASS;
        }

        SetIoStatus(ioStatus, X_STATUS_SUCCESS, written);
        return X_STATUS_SUCCESS;
    }

    // Returns one FILE_DIRECTORY_INFORMATION entry per call: next entry offset, index, four times, end of file,
    // allocation size, attributes, name length and name.
    uint32_t NtQueryDirectoryFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
        uint8_t* information, uint32_t length, XANSI_STRING* fileName, uint32_t restartScan)
    {
        auto file = LookupHandle<FileObject>(handle, ObjectType::File);
        if (!file || !file->directory)
            return X_STATUS_INVALID_HANDLE;

        std::string pattern = fileName != nullptr ? ReadAnsiString(fileName) : "*";
        if (!file->listed || restartScan)
        {
            file->entries = file->file->List();
            file->nextEntry = 0;
            file->listed = true;
        }

        while (file->nextEntry < file->entries.size())
        {
            const auto& entry = file->entries[file->nextEntry++];
            if (!MatchPattern(pattern, entry.name))
                continue;

            uint32_t size = 0x40 + uint32_t(entry.name.size());
            if (size > length)
            {
                file->nextEntry--;
                SetIoStatus(ioStatus, X_STATUS_BUFFER_TOO_SMALL, 0);
                return X_STATUS_BUFFER_TOO_SMALL;
            }

            memset(information, 0, 0x40);
            auto* fields = reinterpret_cast<be<uint64_t>*>(information + 0x08);
            for (int i = 0; i < 4; i++)
                fields[i] = uint64_t(entry.info.writeTime);
            fields[4] = entry.info.size;
            fields[5] = AllocationSize(entry.info.size);
            *reinterpret_cast<be<uint32_t>*>(information + 0x04) = uint32_t(file->nextEntry - 1);
            *reinterpret_cast<be<uint32_t>*>(information + 0x38) = Attributes(entry.info);
            *reinterpret_cast<be<uint32_t>*>(information + 0x3C) = uint32_t(entry.name.size());
            memcpy(information + 0x40, entry.name.data(), entry.name.size());

            SetIoStatus(ioStatus, X_STATUS_SUCCESS, size);
            SignalCompletion(event, apcRoutine);
            return X_STATUS_SUCCESS;
        }

        SetIoStatus(ioStatus, X_STATUS_NO_MORE_FILES, 0);
        SignalCompletion(event, apcRoutine);
        return X_STATUS_NO_MORE_FILES;
    }

    // ---- Symbolic links ----

    uint32_t ObCreateSymbolicLink(XANSI_STRING* link, XANSI_STRING* target)
    {
        auto linkName = ReadAnsiString(link);
        auto targetName = ReadAnsiString(target);
        LOGF_INFO("{} -> {}", linkName, targetName);
        vfs::CreateSymbolicLink(linkName, targetName);
        return X_STATUS_SUCCESS;
    }

    uint32_t ObDeleteSymbolicLink(XANSI_STRING* link)
    {
        vfs::DeleteSymbolicLink(ReadAnsiString(link));
        return X_STATUS_SUCCESS;
    }

    uint32_t ObIsTitleObject(uint32_t object)
    {
        return true;
    }

    // ---- I/O manager, used by titles that implement their own device drivers ----

    uint32_t IoCreateDevice(uint32_t driverObject, uint32_t extensionSize, XANSI_STRING* name, uint32_t type,
        uint32_t characteristics, be<uint32_t>* deviceObject)
    {
        LOGF_WARNING("device {} is not supported", name != nullptr ? ReadAnsiString(name) : "(unnamed)");
        if (deviceObject != nullptr)
            *deviceObject = 0;

        return X_STATUS_NOT_SUPPORTED;
    }

    void IoDeleteDevice(uint32_t deviceObject)
    {
    }

    uint32_t IoInvalidDeviceRequest(uint32_t deviceObject, uint32_t irp)
    {
        return X_STATUS_NOT_SUPPORTED;
    }

    void IoCompleteRequest(uint32_t irp, uint32_t priorityBoost)
    {
        LOG_UNIMPLEMENTED();
    }

    uint32_t IoCheckShareAccess(uint32_t desiredAccess, uint32_t desiredShareAccess, uint32_t fileObject, uint32_t shareAccess, uint32_t update)
    {
        return X_STATUS_SUCCESS;
    }

    void IoSetShareAccess(uint32_t desiredAccess, uint32_t desiredShareAccess, uint32_t fileObject, uint32_t shareAccess)
    {
    }

    void IoRemoveShareAccess(uint32_t fileObject, uint32_t shareAccess)
    {
    }

    // STFS packages (downloadable content). Content is mounted as folders instead.
    uint32_t StfsCreateDevice(uint32_t unknown1, uint32_t unknown2, uint32_t unknown3, uint32_t unknown4,
        uint32_t unknown5, uint32_t unknown6, uint32_t unknown7, uint32_t unknown8)
    {
        LOGF_WARNING("STFS packages are not supported");
        return X_STATUS_NOT_SUPPORTED;
    }

    uint32_t StfsControlDevice(uint32_t device, uint32_t controlCode, uint32_t input, uint32_t inputSize, uint32_t output, uint32_t outputSize)
    {
        return X_STATUS_NOT_SUPPORTED;
    }
}

AR_IMPORT(NtCreateFile);
AR_IMPORT(NtOpenFile);
AR_IMPORT(NtReadFile);
AR_IMPORT(NtReadFileScatter);
AR_IMPORT(NtWriteFile);
AR_IMPORT(NtWriteFileGather);
AR_IMPORT(NtFlushBuffersFile);
AR_IMPORT(NtQueryFullAttributesFile);
AR_IMPORT(NtQueryInformationFile);
AR_IMPORT(NtSetInformationFile);
AR_IMPORT(NtQueryVolumeInformationFile);
AR_IMPORT(NtQueryDirectoryFile);
AR_IMPORT(ObCreateSymbolicLink);
AR_IMPORT(ObDeleteSymbolicLink);
AR_IMPORT(ObIsTitleObject);
AR_IMPORT(IoCreateDevice);
AR_IMPORT(IoDeleteDevice);
AR_IMPORT(IoInvalidDeviceRequest);
AR_IMPORT(IoCompleteRequest);
AR_IMPORT(IoCheckShareAccess);
AR_IMPORT(IoSetShareAccess);
AR_IMPORT(IoRemoveShareAccess);
AR_IMPORT(StfsCreateDevice);
AR_IMPORT(StfsControlDevice);
