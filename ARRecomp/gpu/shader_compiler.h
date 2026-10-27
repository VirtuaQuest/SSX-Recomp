#pragma once

// Compiles HLSL to DXIL with DXC (dxcompiler.dll, from the Windows SDK, next to the executable). Results are cached
// on disk by source hash, so shaders compile once.
namespace gpu
{
    void SetShaderCacheFolder(const std::filesystem::path& folder);

    // target is a shader model profile such as "vs_6_0". Returns an empty vector on failure (logged).
    std::vector<uint8_t> CompileHlsl(const std::string& source, const wchar_t* target, const char* debugName);
}
