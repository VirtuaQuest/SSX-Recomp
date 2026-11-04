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

    // Diagnostics: whether the current frame is traced (AR_TRACE_FRAME / AR_TRACE_FRAMES).
    bool TraceThisFrame();

    // Saves the front buffer every 300 frames (AR_SCREENSHOT_INTERVAL), for debugging.
    void EnableScreenshots(const std::filesystem::path& folder);

    // Pipeline states are kept in a D3D12 pipeline library saved to this file, so later runs don't recreate them.
    void SetPipelineCacheFile(const std::filesystem::path& path);

    // Frames presented so far.
    uint64_t GetFrameNumber();

    // Drawn on top of the picture: 1280 x 720 coordinates, an item of the overlay's art (see mkb_art.inl; -1 solid colour,
    // -2 and -3 the keyboard and controller pages; -4, -5 and -6 a rectangle, in texels, of the title's fonts and small icons; -7 and -8 the same of the keyboard page's lit keys and font, -9 of the title's stencil letters) and a colour.
    struct OverlayQuad
    {
        float x, y, width, height;
        int item;
        float color[4];
        float texels[4] = {};
        bool sharpen = false; // A soft letter with hard edges.
    };

    // Whether the title's fonts for the Options screen have been loaded (it has to be showing for that).
    bool GameFontsAvailable();
    bool GameIconsAvailable();
    bool TitleFontsAvailable(); // The stencil letters for the title screen's words.

    // Saves the next presented frame to screenshots/<name>.bmp (debugging remote control, AR_REMOTE).
    void RequestScreenshot(const std::string& name);
}
