#pragma once

struct LoadedModule
{
    uint32_t imageBase{};
    uint32_t imageSize{};
    uint32_t entryPoint{};
    uint32_t xexHeader{};     // Guest copy of the XEX header, for RtlImageXexHeaderField.
    uint32_t ldrEntry{};      // Guest LDR_DATA_TABLE_ENTRY, the module handle.
    uint32_t titleId{};
};

extern LoadedModule g_module;

// Loads the executable into guest memory, resolves its kernel imports and reads its TLS and stack settings.
bool LoadExecutable(const std::filesystem::path& path);

// Guest address of an optional XEX header field: the value itself for inline keys, otherwise its data.
uint32_t FindXexHeaderField(uint32_t xexHeader, uint32_t key);

// Guest address of the thunk for an import of the executable, by library (xam.xex or xboxkrnl.exe) and ordinal.
// Returns 0 when the executable does not import it.
uint32_t FindImportThunk(bool xam, uint32_t ordinal);
