#pragma once

#include "renderer.h"

namespace xe::gpu
{
    class Shader;
}

// Direct3D 12 implementation of the renderer.
//
// EDRAM is emulated with one host render target per EDRAM base, surface pitch and format; resolves copy from them
// into host textures registered at the destination address, which texture fetches and the front buffer use
// directly instead of going through guest memory. Other textures are decoded from guest memory and re-uploaded
// when their contents change.
namespace gpu::d3d12
{
    struct HostShader
    {
        xe::gpu::Shader* shader;
        const std::vector<uint8_t>* code; // DXIL.
        uint64_t hash;
    };

    void Draw(const be<uint32_t>* registers, const DrawCall& draw, const HostShader& vertexShader, const HostShader* pixelShader);
    void Present(const be<uint32_t>* registers, const SwapInfo& swap);

    // Saves the front buffer every 300 frames (AR_SCREENSHOT_INTERVAL), for debugging.
    void EnableScreenshots(const std::filesystem::path& folder);

    // Pipeline states are kept in a D3D12 pipeline library saved to this file, so later runs don't recreate them.
    void SetPipelineCacheFile(const std::filesystem::path& path);

    // Frames presented so far.
    uint64_t GetFrameNumber();
}
