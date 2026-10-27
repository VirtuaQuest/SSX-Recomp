// Shim: the file helpers the shader code uses.
#pragma once
#include <cstdio>
#include <filesystem>

namespace xe::filesystem
{
    inline FILE* OpenFile(const std::filesystem::path& path, const char* mode)
    {
        return _wfopen(path.c_str(), std::wstring(mode, mode + strlen(mode)).c_str());
    }
}
