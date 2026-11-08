#pragma once

#include <dxgiformat.h>

// Decodes Xenos textures from guest memory into host upload data: untiles, byte swaps, and converts formats the
// host has no equivalent for (16 bit packed colors become RGBA8). Components keep the guest order (X in red), so
// the fetch constant swizzle applies unchanged.
namespace gpu
{
    struct TextureFetch
    {
        uint32_t words[6];

        uint32_t BaseAddress() const;  // Physical.
        uint32_t Format() const { return words[1] & 0x3F; }
        uint32_t Endian() const { return (words[1] >> 6) & 3; }
        bool Tiled() const { return words[0] >> 31; }
        uint32_t Pitch() const { return ((words[0] >> 22) & 0x1FF) << 5; } // Texels.
        uint32_t Dimension() const { return (words[5] >> 9) & 3; }          // 0: 1D, 1: 2D, 2: 3D, 3: cube.
        uint32_t Width() const;
        uint32_t Height() const;
        uint32_t Depth() const;
        uint32_t Swizzle() const { return (words[3] >> 1) & 0xFFF; }
        uint32_t MipMaxLevel() const { return (words[4] >> 6) & 0xF; }
        uint32_t MipAddress() const;   // Physical, 0 without one.
    };

    struct DecodedTexture
    {
        DXGI_FORMAT format;
        uint32_t width;
        uint32_t height;
        uint32_t depth;       // Slices: 3D depth, or 6 for cube maps.
        uint32_t rowPitch;    // Bytes per row of blocks in data.
        uint32_t rowCount;    // Rows of blocks per slice.
        std::vector<uint8_t> data;

        // Smaller levels (1 and on) of 2D textures, as many as the guest stores apart from the packed ones.
        struct Level
        {
            uint32_t width, height, rowPitch, rowCount;
            std::vector<uint8_t> data;
        };
        std::vector<Level> mips;
    };

    // Host format for a Xenos texture format, or DXGI_FORMAT_UNKNOWN if not supported.
    DXGI_FORMAT GetHostTextureFormat(uint32_t format);

    // Size in bytes of the base level in guest memory, for change detection.
    uint32_t GetGuestTextureSize(const TextureFetch& fetch);

    bool DecodeTexture(const TextureFetch& fetch, DecodedTexture& out);
}
