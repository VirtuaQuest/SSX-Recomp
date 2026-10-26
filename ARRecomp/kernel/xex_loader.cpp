#include <stdafx.h>
#include "xex_loader.h"
#include "heap.h"
#include "memory.h"
#include "variables.h"
#include <cpu/guest_thread.h>
#include <file.h>
#include <image.h>
#include <xex.h>

LoadedModule g_module;

// Import thunks by library (true for xam.xex) and ordinal, for XexGetProcedureAddress.
static std::map<std::pair<bool, uint32_t>, uint32_t> g_importThunks;

struct ExportInfo
{
    const char* name;
    bool variable;
};

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), { #NAME, std::string_view(#TYPE) == "kVariable" } }
static const std::unordered_map<uint32_t, ExportInfo> g_kernelExports = {
#include "xbox/xboxkrnl_table.inc"
};
static const std::unordered_map<uint32_t, ExportInfo> g_xamExports = {
#include "xbox/xam_table.inc"
};
#undef XE_EXPORT

uint32_t FindXexHeaderField(uint32_t xexHeader, uint32_t key)
{
    auto* header = g_memory.Translate<Xex2Header>(xexHeader);
    auto* entries = reinterpret_cast<const Xex2OptHeader*>(header + 1);
    for (uint32_t i = 0; i < header->headerCount; i++)
    {
        if (entries[i].key != key)
            continue;

        if ((key & 0xFF) == 0 || (key & 0xFF) == 1)
            return g_memory.MapVirtual(&entries[i].value);

        return xexHeader + entries[i].offset;
    }

    return 0;
}

// XenonUtils' parser leaves every import record in host byte order. Records of type 1 (function thunks) are
// overwritten with thunk code, which reads back as 0x00000060; the recompiler maps their addresses to the import
// implementations, and the image symbols name them "__imp__<name>".
static void ResolveImports(const uint8_t* xex, const Image& image)
{
    auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
    if (imports == nullptr)
        return;

    std::unordered_map<std::string_view, uint32_t> thunks;
    for (const auto& symbol : image.symbols)
    {
        if (symbol.name.rfind("__imp__", 0) == 0)
            thunks.emplace(std::string_view(symbol.name).substr(7), uint32_t(symbol.address));
    }

    std::vector<std::string_view> libraryNames;
    const char* strings = reinterpret_cast<const char*>(imports + 1);
    size_t offset = 0;
    for (size_t i = 0; i < imports->numImports; i++)
    {
        libraryNames.emplace_back(strings + offset);
        offset += (libraryNames.back().size() + 1 + 3) & ~3;
    }

    auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
    for (const auto& libraryName : libraryNames)
    {
        const auto& exports = libraryName == "xam.xex" ? g_xamExports : g_kernelExports;
        auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);

        for (size_t i = 0; i < library->numberOfImports; i++)
        {
            uint32_t slot = descriptors[i].firstThunk;
            uint32_t record = *g_memory.Translate<uint32_t>(slot);
            if (record == 0x00000060 || (record >> 24) != 0)
                continue;

            // Import table slot. Variables receive the variable's address; functions receive their thunk, so taking
            // a kernel function's address and calling it indirectly still reaches the implementation.
            uint32_t ordinal = record & 0xFFFF;
            auto exportInfo = exports.find(ordinal);
            auto* slotPtr = g_memory.Translate<be<uint32_t>>(slot);

            if (exportInfo == exports.end())
            {
                LOGF_WARNING("unknown import {} ordinal {}", libraryName, ordinal);
                *slotPtr = 0;
            }
            else if (exportInfo->second.variable)
            {
                uint32_t address = GetKernelVariable(exportInfo->second.name);
                if (address == 0)
                    LOGF_WARNING("unimplemented kernel variable {}", exportInfo->second.name);

                *slotPtr = address;
            }
            else
            {
                auto thunk = thunks.find(exportInfo->second.name);
                *slotPtr = thunk != thunks.end() ? thunk->second : 0;
                if (thunk != thunks.end())
                    g_importThunks[{ libraryName == "xam.xex", ordinal }] = thunk->second;
            }
        }

        library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
}

bool LoadExecutable(const std::filesystem::path& path)
{
    auto file = LoadFile(path);
    if (file.empty())
    {
        LOGF_ERROR("failed to read {}", path.string());
        return false;
    }

    // XenonUtils decrypts and decompresses the image.
    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        LOGF_ERROR("failed to parse {}", path.string());
        return false;
    }

    g_module.imageBase = uint32_t(image.base);
    g_module.imageSize = image.size;
    g_module.entryPoint = uint32_t(image.entry_point);
    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);

    // Keep a guest copy of the XEX header for RtlImageXexHeaderField and code that parses it directly.
    auto* xexHeader = reinterpret_cast<const Xex2Header*>(file.data());
    g_module.xexHeader = g_runtimeHeap.AllocGuest(xexHeader->headerSize);
    memcpy(g_memory.Translate(g_module.xexHeader), file.data(), xexHeader->headerSize);

    // LDR_DATA_TABLE_ENTRY, used as the module handle.
    g_module.ldrEntry = g_runtimeHeap.AllocGuest(0x80);
    auto writeLdr = [](uint32_t offset, uint32_t value) { *g_memory.Translate<be<uint32_t>>(g_module.ldrEntry + offset) = value; };
    writeLdr(0x18, g_module.imageBase);  // DllBase
    writeLdr(0x1C, g_module.imageBase);  // ImageBase
    writeLdr(0x20, g_module.imageSize);  // SizeOfImage
    writeLdr(0x3C, g_module.entryPoint); // EntryPoint
    writeLdr(0x58, g_module.xexHeader);  // XexHeaderBase

    if (auto* tls = reinterpret_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_TLS_INFO)))
    {
        g_tlsInfo.slotCount = tls[0];
        g_tlsInfo.rawDataAddress = tls[1];
        g_tlsInfo.dataSize = tls[2];
        g_tlsInfo.rawDataSize = tls[3];
    }

    if (auto* stackSize = reinterpret_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE)))
        g_defaultStackSize = *stackSize;

    if (auto* execution = reinterpret_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_EXECUTION_INFO)))
        g_module.titleId = execution[3];

    ResolveImports(file.data(), image);

    LOGF_INFO("loaded image 0x{:08X}-0x{:08X}, entry 0x{:08X}, title {:08X}, TLS {} slots + {} bytes, stack 0x{:X}",
        g_module.imageBase, g_module.imageBase + g_module.imageSize, g_module.entryPoint, g_module.titleId,
        g_tlsInfo.slotCount, g_tlsInfo.dataSize, g_defaultStackSize);

    return true;
}

uint32_t FindImportThunk(bool xam, uint32_t ordinal)
{
    auto it = g_importThunks.find({ xam, ordinal });
    return it != g_importThunks.end() ? it->second : 0;
}
