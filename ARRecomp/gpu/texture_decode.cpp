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
        uint32_t bppLog2 = std::countr_zero(info.bytesPerBlock);
        uint8_t block[16];

        for (uint32_t z = 0; z < slices; z++)
        {
            const uint8_t* slice = base + size_t(layout.sliceBytes) * z;
            uint8_t* destSlice = out.data.data() + size_t(out.rowPitch) * out.rowCount * z;

            for (uint32_t y = 0; y < layout.heightBlocks; y++)
            {
                uint8_t* destRow = destSlice + size_t(out.rowPitch) * y;
                for (uint32_t x = 0; x < layout.widthBlocks; x++)
                {
                    size_t offset = fetch.Tiled()
                        ? size_t(xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), layout.pitchBlocks, bppLog2))
                        : size_t(y) * layout.rowPitchBytes + size_t(x) * info.bytesPerBlock;

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

        return true;
    }
}
