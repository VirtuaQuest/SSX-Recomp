#include <stdafx.h>
#include "texture_decode.h"
#include <kernel/memory.h>
#include "xenia/gpu/texture_address.h"

namespace gpu
{
    enum class Conversion
    {
        None,
        R5G6B5,   // X in bits 0-4, Y 5-10, Z 11-15.
        R5G5B5A1, // X in bits 0-4, Y 5-9, Z 10-14, W 15.
        R4G4B4A4, // X in bits 0-3, Y 4-7, Z 8-11, W 12-15.
    };

    struct FormatInfo
    {
        DXGI_FORMAT host;
        uint32_t blockSize;      // 1 or 4 texels square.
        uint32_t bytesPerBlock;
        Conversion conversion;
    };

    static bool GetFormatInfo(uint32_t format, FormatInfo& info)
    {
        switch (format)
        {
        case 2: case 8: case 9: info = { DXGI_FORMAT_R8_UNORM, 1, 1 }; return true;              // 8, 8_A, 8_B
        case 3: info = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, Conversion::R5G5B5A1 }; return true;   // 1_5_5_5
        case 4: info = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, Conversion::R5G6B5 }; return true;     // 5_6_5
        case 6: case 14: case 50: info = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4 }; return true;      // 8_8_8_8
        case 7: case 54: info = { DXGI_FORMAT_R10G10B10A2_UNORM, 1, 4 }; return true;            // 2_10_10_10
        case 10: info = { DXGI_FORMAT_R8G8_UNORM, 1, 2 }; return true;                           // 8_8
        case 15: info = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, Conversion::R4G4B4A4 }; return true;  // 4_4_4_4
        case 18: case 51: info = { DXGI_FORMAT_BC1_UNORM, 4, 8 }; return true;                   // DXT1
        case 19: case 52: info = { DXGI_FORMAT_BC2_UNORM, 4, 16 }; return true;                  // DXT2_3
        case 20: case 53: info = { DXGI_FORMAT_BC3_UNORM, 4, 16 }; return true;                  // DXT4_5
        case 24: info = { DXGI_FORMAT_R16_UNORM, 1, 2 }; return true;                            // 16
        case 25: info = { DXGI_FORMAT_R16G16_UNORM, 1, 4 }; return true;                         // 16_16
        case 26: info = { DXGI_FORMAT_R16G16B16A16_UNORM, 1, 8 }; return true;                   // 16_16_16_16
        case 30: info = { DXGI_FORMAT_R16_FLOAT, 1, 2 }; return true;                            // 16_FLOAT
        case 31: info = { DXGI_FORMAT_R16G16_FLOAT, 1, 4 }; return true;                         // 16_16_FLOAT
        case 32: info = { DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8 }; return true;                   // 16_16_16_16_FLOAT
        case 36: info = { DXGI_FORMAT_R32_FLOAT, 1, 4 }; return true;                            // 32_FLOAT
        case 37: info = { DXGI_FORMAT_R32G32_FLOAT, 1, 8 }; return true;                         // 32_32_FLOAT
        case 38: info = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16 }; return true;                  // 32_32_32_32_FLOAT
        case 49: info = { DXGI_FORMAT_BC5_UNORM, 4, 16 }; return true;                           // DXN
        case 59: info = { DXGI_FORMAT_BC4_UNORM, 4, 8 }; return true;                            // DXT5A
        default: return false;
        }
    }

    uint32_t TextureFetch::BaseAddress() const
    {
        uint32_t address = words[1] & 0xFFFFF000;
        return IsPhysicalAddress(address) ? VirtualToPhysical(address) : address & (GUEST_PHYSICAL_SIZE - 1);
    }

    uint32_t TextureFetch::MipAddress() const
    {
        uint32_t address = words[5] & 0xFFFFF000;
        if (address == 0)
            return 0;
        return IsPhysicalAddress(address) ? VirtualToPhysical(address) : address & (GUEST_PHYSICAL_SIZE - 1);
    }

    uint32_t TextureFetch::Width() const
    {
        return Dimension() == 0 ? (words[2] & 0xFFFFFF) + 1 : Dimension() == 2 ? (words[2] & 0x7FF) + 1 : (words[2] & 0x1FFF) + 1;
    }

    uint32_t TextureFetch::Height() const
    {
        switch (Dimension())
        {
        case 0: return 1;
        case 2: return ((words[2] >> 11) & 0x7FF) + 1;
        default: return ((words[2] >> 13) & 0x1FFF) + 1;
        }
    }

    uint32_t TextureFetch::Depth() const
    {
        switch (Dimension())
        {
        case 2: return (words[2] >> 22) + 1;
        case 3: return 6;
        default: return 1;
        }
    }

    DXGI_FORMAT GetHostTextureFormat(uint32_t format)
    {
        FormatInfo info;
        return GetFormatInfo(format, info) ? info.host : DXGI_FORMAT_UNKNOWN;
    }

    // Layout of the base level in guest memory, in blocks.
    struct GuestLayout
    {
        uint32_t widthBlocks;
        uint32_t heightBlocks;
        uint32_t pitchBlocks;     // Aligned storage pitch.
        uint32_t heightAligned;   // Aligned storage height (tiled).
        uint32_t rowPitchBytes;   // Linear textures.
        uint32_t sliceBytes;
    };

    static GuestLayout GetGuestLayout(const TextureFetch& fetch, const FormatInfo& info)
    {
        GuestLayout layout;
        layout.widthBlocks = (fetch.Width() + info.blockSize - 1) / info.blockSize;
        layout.heightBlocks = (fetch.Height() + info.blockSize - 1) / info.blockSize;
        uint32_t pitch = std::max(fetch.Pitch(), fetch.Width());
        layout.pitchBlocks = (pitch + info.blockSize - 1) / info.blockSize;

        if (fetch.Tiled())
        {
            layout.pitchBlocks = (layout.pitchBlocks + 31) & ~31u;
            layout.heightAligned = (layout.heightBlocks + 31) & ~31u;
            layout.rowPitchBytes = layout.pitchBlocks * info.bytesPerBlock;
            layout.sliceBytes = (layout.rowPitchBytes * layout.heightAligned + 4095) & ~4095u;
        }
        else
        {
            layout.heightAligned = layout.heightBlocks;
            layout.rowPitchBytes = (layout.pitchBlocks * info.bytesPerBlock + 255) & ~255u;
            layout.sliceBytes = (layout.rowPitchBytes * ((layout.heightBlocks + 31) & ~31u) + 4095) & ~4095u;
        }

        return layout;
    }

    uint32_t GetGuestTextureSize(const TextureFetch& fetch)
    {
        FormatInfo info;
        if (!GetFormatInfo(fetch.Format(), info))
            return 0;

        return GetGuestLayout(fetch, info).sliceBytes * (fetch.Dimension() >= 2 ? fetch.Depth() : 1);
    }

    static void SwapBlock(uint8_t* block, uint32_t size, uint32_t endian)
    {
        switch (endian)
        {
        case 1: // 8 in 16
            for (uint32_t i = 0; i + 1 < size; i += 2)
                std::swap(block[i], block[i + 1]);
            break;
        case 2: // 8 in 32
            for (uint32_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(block[i], block[i + 3]);
                std::swap(block[i + 1], block[i + 2]);
            }
            break;
        case 3: // 16 in 32
            for (uint32_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(block[i], block[i + 2]);
                std::swap(block[i + 1], block[i + 3]);
            }
            break;
        }
    }

    static uint32_t Expand(uint32_t value, uint32_t bits)
    {
        return (value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
    }

    static void Convert(Conversion conversion, const uint8_t* source, uint8_t* dest)
    {
        uint16_t v = uint16_t(source[0] | (source[1] << 8));
        switch (conversion)
        {
        case Conversion::R5G6B5:
            dest[0] = uint8_t(Expand(v & 31, 5));
            dest[1] = uint8_t(Expand((v >> 5) & 63, 6));
            dest[2] = uint8_t(Expand(v >> 11, 5));
            dest[3] = 255;
            break;
        case Conversion::R5G5B5A1:
            dest[0] = uint8_t(Expand(v & 31, 5));
            dest[1] = uint8_t(Expand((v >> 5) & 31, 5));
            dest[2] = uint8_t(Expand((v >> 10) & 31, 5));
            dest[3] = (v >> 15) ? 255 : 0;
            break;
        case Conversion::R4G4B4A4:
            dest[0] = uint8_t(Expand(v & 15, 4));
            dest[1] = uint8_t(Expand((v >> 4) & 15, 4));
            dest[2] = uint8_t(Expand((v >> 8) & 15, 4));
            dest[3] = uint8_t(Expand(v >> 12, 4));
            break;
        default:
            break;
        }
    }

    // ---- Block compressed textures whose top level isn't a whole number of blocks ----
    //
    // D3D12 needs block compressed textures to be multiples of 4 texels across and down. Padding them would stretch
    // what the title samples by up to a texel, so such textures are decompressed to RGBA8 instead (BC4 to red, BC5 to
    // red and green, as their own formats sample). AR_NO_BC_DECOMPRESS leaves them as they were (they then fail to
    // be created).

    static void DecodeColorBlock(const uint8_t* block, uint8_t (*texels)[4], bool fourColors)
    {
        auto expand = [](uint16_t c, uint8_t* out)
            {
                out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
                out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
                out[2] = uint8_t((c & 31) * 255 / 31);
                out[3] = 255;
            };
        uint16_t c0 = uint16_t(block[0] | (block[1] << 8)), c1 = uint16_t(block[2] | (block[3] << 8));
        uint8_t colors[4][4];
        expand(c0, colors[0]);
        expand(c1, colors[1]);
        for (int i = 0; i < 3; i++)
        {
            if (fourColors || c0 > c1)
            {
                colors[2][i] = uint8_t((2 * colors[0][i] + colors[1][i]) / 3);
                colors[3][i] = uint8_t((colors[0][i] + 2 * colors[1][i]) / 3);
            }
            else
            {
                colors[2][i] = uint8_t((colors[0][i] + colors[1][i]) / 2);
                colors[3][i] = 0;
            }
        }
        colors[2][3] = 255;
        colors[3][3] = (fourColors || c0 > c1) ? 255 : 0;
        uint32_t indices = uint32_t(block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24));
        for (int t = 0; t < 16; t++)
            memcpy(texels[t], colors[(indices >> (t * 2)) & 3], 4);
    }

    // BC3 alpha, BC4 and BC5 channels: two endpoints and 3 bit indices.
    static void DecodeInterpolatedBlock(const uint8_t* block, uint8_t (*texels)[4], int channel)
    {
        uint8_t values[8];
        values[0] = block[0];
        values[1] = block[1];
        if (values[0] > values[1])
        {
            for (int i = 1; i < 7; i++)
                values[i + 1] = uint8_t(((7 - i) * values[0] + i * values[1]) / 7);
        }
        else
        {
            for (int i = 1; i < 5; i++)
                values[i + 1] = uint8_t(((5 - i) * values[0] + i * values[1]) / 5);
            values[6] = 0;
            values[7] = 255;
        }
        uint64_t indices = 0;
        for (int i = 0; i < 6; i++)
            indices |= uint64_t(block[2 + i]) << (8 * i);
        for (int t = 0; t < 16; t++)
            texels[t][channel] = values[(indices >> (t * 3)) & 7];
    }

    static bool DecompressBlocks(DecodedTexture& texture)
    {
        uint32_t blockBytes;
        switch (texture.format)
        {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC4_UNORM: blockBytes = 8; break;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC5_UNORM: blockBytes = 16; break;
        default: return false;
        }

        uint32_t widthBlocks = (texture.width + 3) / 4;
        uint32_t heightBlocks = texture.rowCount;
        // Rows of upload data have to start on 256 byte boundaries (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
        uint32_t rowPitch = (texture.width * 4 + 255) & ~255u;
        std::vector<uint8_t> rgba(size_t(rowPitch) * texture.height * texture.depth);
        uint8_t texels[16][4];
        for (uint32_t z = 0; z < texture.depth; z++)
        {
            const uint8_t* source = texture.data.data() + size_t(texture.rowPitch) * texture.rowCount * z;
            uint8_t* dest = rgba.data() + size_t(rowPitch) * texture.height * z;
            for (uint32_t by = 0; by < heightBlocks; by++)
            {
                for (uint32_t bx = 0; bx < widthBlocks; bx++)
                {
                    const uint8_t* block = source + size_t(texture.rowPitch) * by + size_t(bx) * blockBytes;
                    switch (texture.format)
                    {
                    case DXGI_FORMAT_BC1_UNORM:
                        DecodeColorBlock(block, texels, false);
                        break;
                    case DXGI_FORMAT_BC2_UNORM:
                        DecodeColorBlock(block + 8, texels, true);
                        for (int t = 0; t < 16; t++)
                            texels[t][3] = uint8_t(((block[t / 2] >> ((t & 1) * 4)) & 15) * 17);
                        break;
                    case DXGI_FORMAT_BC3_UNORM:
                        DecodeColorBlock(block + 8, texels, true);
                        DecodeInterpolatedBlock(block, texels, 3);
                        break;
                    case DXGI_FORMAT_BC4_UNORM:
                        for (auto& texel : texels)
                        {
                            texel[1] = texel[2] = 0;
                            texel[3] = 255;
                        }
                        DecodeInterpolatedBlock(block, texels, 0);
                        break;
                    default: // BC5
                        for (auto& texel : texels)
                        {
                            texel[2] = 0;
                            texel[3] = 255;
                        }
                        DecodeInterpolatedBlock(block, texels, 0);
                        DecodeInterpolatedBlock(block + 8, texels, 1);
                        break;
                    }
                    for (uint32_t ty = 0; ty < 4; ty++)
                    {
                        uint32_t y = by * 4 + ty;
                        if (y >= texture.height)
                            break;
                        for (uint32_t tx = 0; tx < 4; tx++)
                        {
                            uint32_t x = bx * 4 + tx;
                            if (x < texture.width)
                                memcpy(dest + size_t(rowPitch) * y + x * 4, texels[ty * 4 + tx], 4);
                        }
                    }
                }
            }
        }
        texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.rowPitch = rowPitch;
        texture.rowCount = texture.height;
        texture.data = std::move(rgba);
        return true;
    }

    // ---- Smaller levels ----
    //
    // A 2D texture's levels 1 and on start at the fetch constant's mip address, one after another, each with power of two
    // dimensions (the base's rounded up, halved per level), rows padded to 32 blocks when tiled (256 bytes when linear),
    // 32 rows of blocks, and 4 KB alignment. The last fetchable level is in word 4. Levels whose smaller side is 16 texels
    // or less are packed together into one tile when the packed flag is set; the chain stops before them here (sampling then
    // clamps at the smallest level there is, 32 texels, which is what matters against aliasing). AR_NO_MIPS leaves textures
    // at their base level.
    static void DecodeMips(const TextureFetch& fetch, const FormatInfo& info, DecodedTexture& out)
    {
        static const bool disabled = AR_ENV("AR_NO_MIPS") != nullptr;
        uint32_t maxLevel = (fetch.words[4] >> 6) & 0xF;
        uint32_t mipBase = fetch.words[5] & 0xFFFFF000;
        if (disabled || fetch.Dimension() != 1 || out.depth != 1 || maxLevel == 0 || mipBase == 0)
            return;
        uint32_t mipAddress = IsPhysicalAddress(mipBase) ? VirtualToPhysical(mipBase) : mipBase & (GUEST_PHYSICAL_SIZE - 1);
        const uint8_t* levels = g_memory.Translate<uint8_t>(PhysicalToVirtual(mipAddress));

        bool packed = ((fetch.words[5] >> 11) & 1) != 0;
        uint32_t hostBytesPerBlock = info.conversion == Conversion::None ? info.bytesPerBlock : 4;
        uint32_t bppLog2 = std::countr_zero(info.bytesPerBlock);
        uint32_t width = std::bit_ceil(fetch.Width()), height = std::bit_ceil(fetch.Height());
        size_t offset = 0;
        uint8_t block[16];
        for (uint32_t level = 1; level <= maxLevel; level++)
        {
            uint32_t levelWidth = std::max(width >> level, 1u), levelHeight = std::max(height >> level, 1u);
            if (packed && std::min(std::bit_width(levelWidth - 1), std::bit_width(levelHeight - 1)) <= 4)
                break;

            uint32_t widthBlocks = (levelWidth + info.blockSize - 1) / info.blockSize;
            uint32_t heightBlocks = (levelHeight + info.blockSize - 1) / info.blockSize;
            uint32_t pitchBlocks, rowPitchBytes;
            size_t levelBytes;
            if (fetch.Tiled())
            {
                pitchBlocks = (widthBlocks + 31) & ~31u;
                rowPitchBytes = pitchBlocks * info.bytesPerBlock;
                levelBytes = (size_t(rowPitchBytes) * ((heightBlocks + 31) & ~31u) + 4095) & ~size_t(4095);
            }
            else
            {
                pitchBlocks = widthBlocks;
                rowPitchBytes = (widthBlocks * info.bytesPerBlock + 255) & ~255u;
                levelBytes = (size_t(rowPitchBytes) * ((heightBlocks + 31) & ~31u) + 4095) & ~size_t(4095);
            }

            DecodedTexture::Level& mip = out.mips.emplace_back();
            mip.width = levelWidth;
            mip.height = levelHeight;
            mip.rowPitch = (widthBlocks * hostBytesPerBlock + 255) & ~255u;
            mip.rowCount = heightBlocks;
            mip.data.assign(size_t(mip.rowPitch) * mip.rowCount, 0);
            const uint8_t* source = levels + offset;
            for (uint32_t y = 0; y < heightBlocks; y++)
            {
                uint8_t* destRow = mip.data.data() + size_t(mip.rowPitch) * y;
                for (uint32_t x = 0; x < widthBlocks; x++)
                {
                    size_t at = fetch.Tiled()
                        ? size_t(xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), pitchBlocks, bppLog2))
                        : size_t(y) * rowPitchBytes + size_t(x) * info.bytesPerBlock;
                    memcpy(block, source + at, info.bytesPerBlock);
                    SwapBlock(block, info.bytesPerBlock, fetch.Endian());
                    uint8_t* dest = destRow + size_t(x) * hostBytesPerBlock;
                    if (info.conversion == Conversion::None)
                        memcpy(dest, block, info.bytesPerBlock);
                    else
                        Convert(info.conversion, block, dest);
                }
            }
            offset += levelBytes;
        }
    }

    bool DecodeTexture(const TextureFetch& fetch, DecodedTexture& out)
    {
        FormatInfo info;
        if (!GetFormatInfo(fetch.Format(), info))
            return false;

        GuestLayout layout = GetGuestLayout(fetch, info);
        uint32_t slices = fetch.Dimension() >= 2 ? fetch.Depth() : 1;
        uint32_t hostBytesPerBlock = info.conversion == Conversion::None ? info.bytesPerBlock : 4;

        out.format = info.host;
        out.width = fetch.Width();
        out.height = fetch.Height();
        out.depth = slices;
        out.rowPitch = (layout.widthBlocks * hostBytesPerBlock + 255) & ~255u;
        out.rowCount = layout.heightBlocks;
        out.data.assign(size_t(out.rowPitch) * out.rowCount * slices, 0);

        const uint8_t* base = g_memory.Translate<uint8_t>(PhysicalToVirtual(fetch.BaseAddress()));

        // Packed mips: textures with a side of 16 texels or less sit 16 texels into their 32x32 tile, below it
        // when wider than tall, else beside it (Xenia's texture_util GetPackedMipOffset, for the base level). Linear
        // textures are laid out the same way, rows of 32.
        uint32_t offsetX = 0, offsetY = 0;
        if ((fetch.words[5] >> 11) & 1)
        {
            uint32_t log2Width = std::bit_width(fetch.Width() - 1), log2Height = std::bit_width(fetch.Height() - 1);
            if (std::min(log2Width, log2Height) <= 4)
            {
                bool wide = log2Width > std::max<uint32_t>(log2Height, 1);
                (wide ? offsetY : offsetX) = 16 / info.blockSize;
            }
        }
        uint32_t bppLog2 = std::countr_zero(info.bytesPerBlock);
        uint8_t block[16];

        // Tiled 3D textures are tiled in 32x16x4 blocks through the whole volume (Xenia's Tiled3D), not slice by slice
        // as 2D: decoded as 2D slices, the 128x128x256 volume SSX's snow terrain looks up came out scrambled, and whole
        // patches of slope got the wrong normals (flat polygons with straight edges). AR_NO_TILED_3D decodes slices as 2D.
        static const bool tiled3dOff = AR_ENV("AR_NO_TILED_3D") != nullptr;
        bool tiled3d = fetch.Tiled() && fetch.Dimension() == 2 && !tiled3dOff;

        for (uint32_t z = 0; z < slices; z++)
        {
            const uint8_t* slice = tiled3d ? base : base + size_t(layout.sliceBytes) * z;
            uint8_t* destSlice = out.data.data() + size_t(out.rowPitch) * out.rowCount * z;

            for (uint32_t y = 0; y < layout.heightBlocks; y++)
            {
                uint8_t* destRow = destSlice + size_t(out.rowPitch) * y;
                for (uint32_t x = 0; x < layout.widthBlocks; x++)
                {
                    size_t offset = tiled3d
                        ? size_t(xe::gpu::texture_address::Tiled3D(int32_t(x + offsetX), int32_t(y + offsetY), int32_t(z), layout.pitchBlocks,
                            layout.heightAligned, bppLog2))
                        : fetch.Tiled()
                        ? size_t(xe::gpu::texture_address::Tiled2D(int32_t(x + offsetX), int32_t(y + offsetY), layout.pitchBlocks, bppLog2))
                        : size_t(y + offsetY) * layout.rowPitchBytes + size_t(x + offsetX) * info.bytesPerBlock;

                    memcpy(block, slice + offset, info.bytesPerBlock);
                    SwapBlock(block, info.bytesPerBlock, fetch.Endian());

                    uint8_t* dest = destRow + size_t(x) * hostBytesPerBlock;
                    if (info.conversion == Conversion::None)
                        memcpy(dest, block, info.bytesPerBlock);
                    else
                        Convert(info.conversion, block, dest);
                }
            }
        }

        static const bool decompress = !AR_ENV("AR_NO_BC_DECOMPRESS");
        if (decompress && ((out.width & 3) != 0 || (out.height & 3) != 0))
        {
            DecompressBlocks(out);
            return true; // Without its smaller levels: they'd need decompressing too.
        }

        DecodeMips(fetch, info, out);
        return true;
    }
}
