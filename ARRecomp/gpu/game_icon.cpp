// The window's icon is the emblem from the title's own logo. It's cut out of the logo's texture the first time the title screen loads that, from the
// player's copy of the game, and kept in the user's folder (user/game_icon.ico), so no art of the game is in this project or its executable.
#include <stdafx.h>
#include "window.h"

namespace gpu::window
{
    static constexpr const wchar_t* ICON_FILE = L"user/game_icon.ico";
    // Where the emblem is on the logo's 1024 x 1024 texture: a square around it.
    static constexpr uint32_t EMBLEM_X = 372, EMBLEM_Y = 267, EMBLEM_SIZE = 232;

    static void Decode565(uint16_t value, uint8_t* rgb)
    {
        rgb[0] = uint8_t(((value >> 11) & 31) * 255 / 31);
        rgb[1] = uint8_t(((value >> 5) & 63) * 255 / 63);
        rgb[2] = uint8_t((value & 31) * 255 / 31);
    }

    // One pixel of a BC3 texture.
    static void ReadBc3Pixel(const uint8_t* data, uint32_t rowPitch, uint32_t x, uint32_t y, uint8_t* rgba)
    {
        const uint8_t* block = data + size_t(y / 4) * rowPitch + size_t(x / 4) * 16;
        uint32_t px = x & 3, py = y & 3, index = py * 4 + px;

        uint8_t a0 = block[0], a1 = block[1];
        uint64_t alphaBits = 0;
        for (int i = 0; i < 6; i++)
            alphaBits |= uint64_t(block[2 + i]) << (8 * i);
        uint32_t alphaCode = uint32_t((alphaBits >> (3 * index)) & 7);
        uint32_t alpha;
        if (alphaCode == 0)
            alpha = a0;
        else if (alphaCode == 1)
            alpha = a1;
        else if (a0 > a1)
            alpha = ((8 - alphaCode) * a0 + (alphaCode - 1) * a1) / 7;
        else if (alphaCode < 6)
            alpha = ((6 - alphaCode) * a0 + (alphaCode - 1) * a1) / 5;
        else
            alpha = alphaCode == 6 ? 0 : 255;

        uint8_t c0[3], c1[3];
        Decode565(uint16_t(block[8] | block[9] << 8), c0);
        Decode565(uint16_t(block[10] | block[11] << 8), c1);
        uint32_t colorBits = uint32_t(block[12]) | uint32_t(block[13]) << 8 | uint32_t(block[14]) << 16 | uint32_t(block[15]) << 24;
        uint32_t colorCode = (colorBits >> (2 * index)) & 3;
        for (int i = 0; i < 3; i++)
        {
            uint32_t value = colorCode == 0 ? c0[i] : colorCode == 1 ? c1[i] : colorCode == 2 ? (2 * c0[i] + c1[i]) / 3 : (c0[i] + 2 * c1[i]) / 3;
            rgba[i] = uint8_t(value);
        }
        rgba[3] = uint8_t(alpha);
    }

    // To another size, averaging samples weighted by their alpha.
    static std::vector<uint8_t> Resize(const std::vector<uint8_t>& source, uint32_t sourceSize, uint32_t size)
    {
        static constexpr uint32_t SAMPLES = 4;
        std::vector<uint8_t> result(size_t(size) * size * 4);
        for (uint32_t y = 0; y < size; y++)
        {
            for (uint32_t x = 0; x < size; x++)
            {
                float sum[4] = {};
                for (uint32_t sy = 0; sy < SAMPLES; sy++)
                {
                    for (uint32_t sx = 0; sx < SAMPLES; sx++)
                    {
                        uint32_t u = std::min(uint32_t((float(x) + (float(sx) + 0.5f) / SAMPLES) * float(sourceSize) / float(size)), sourceSize - 1);
                        uint32_t v = std::min(uint32_t((float(y) + (float(sy) + 0.5f) / SAMPLES) * float(sourceSize) / float(size)), sourceSize - 1);
                        const uint8_t* pixel = source.data() + (size_t(v) * sourceSize + u) * 4;
                        float alpha = float(pixel[3]);
                        sum[0] += float(pixel[0]) * alpha;
                        sum[1] += float(pixel[1]) * alpha;
                        sum[2] += float(pixel[2]) * alpha;
                        sum[3] += alpha;
                    }
                }
                uint8_t* out = result.data() + (size_t(y) * size + x) * 4;
                for (int i = 0; i < 3; i++)
                    out[i] = sum[3] > 0.0f ? uint8_t(sum[i] / sum[3]) : 0;
                out[3] = uint8_t(sum[3] / float(SAMPLES * SAMPLES));
            }
        }
        return result;
    }

    bool NeedsGameIcon()
    {
        static const bool disabled = AR_ENV("AR_NO_GAME_ICON") != nullptr;
        std::error_code ec;
        return !disabled && !std::filesystem::exists(ICON_FILE, ec);
    }

    void SaveGameIcon(const uint8_t* bc3, uint32_t rowPitch)
    {
        // The emblem is orange-red, and the yellow strip and black letters of the logo run into it: what isn't its colour goes.
        std::vector<uint8_t> emblem(size_t(EMBLEM_SIZE) * EMBLEM_SIZE * 4);
        for (uint32_t y = 0; y < EMBLEM_SIZE; y++)
        {
            for (uint32_t x = 0; x < EMBLEM_SIZE; x++)
            {
                uint8_t* pixel = emblem.data() + (size_t(y) * EMBLEM_SIZE + x) * 4;
                ReadBc3Pixel(bc3, rowPitch, EMBLEM_X + x, EMBLEM_Y + y, pixel);
                bool emblemColor = pixel[0] >= 60 && float(pixel[0]) >= float(pixel[1]) * 1.55f;
                if (!emblemColor)
                    pixel[3] = 0;
            }
        }

        static constexpr uint32_t SIZES[] = { 256, 64, 48, 32, 24, 16 };
        std::vector<uint8_t> file(6 + std::size(SIZES) * 16);
        auto put16 = [&](size_t at, uint32_t value) { file[at] = uint8_t(value); file[at + 1] = uint8_t(value >> 8); };
        auto put32 = [&](size_t at, uint32_t value) { put16(at, value & 0xFFFF); put16(at + 2, value >> 16); };
        put16(2, 1);
        put16(4, uint32_t(std::size(SIZES)));
        for (size_t i = 0; i < std::size(SIZES); i++)
        {
            uint32_t size = SIZES[i];
            std::vector<uint8_t> pixels = Resize(emblem, EMBLEM_SIZE, size);
            uint32_t maskRow = ((size + 31) / 32) * 4;
            uint32_t bytes = 40 + size * size * 4 + maskRow * size;
            size_t entry = 6 + i * 16;
            file[entry] = uint8_t(size == 256 ? 0 : size);
            file[entry + 1] = uint8_t(size == 256 ? 0 : size);
            put16(entry + 4, 1);
            put16(entry + 6, 32);
            put32(entry + 8, bytes);
            put32(entry + 12, uint32_t(file.size()));

            size_t at = file.size();
            file.resize(at + bytes);
            put32(at, 40);
            put32(at + 4, size);
            put32(at + 8, size * 2);
            put16(at + 12, 1);
            put16(at + 14, 32);
            size_t out = at + 40;
            for (uint32_t y = 0; y < size; y++)
            {
                const uint8_t* row = pixels.data() + size_t(size - 1 - y) * size * 4;
                for (uint32_t x = 0; x < size; x++, out += 4)
                {
                    file[out + 0] = row[x * 4 + 2];
                    file[out + 1] = row[x * 4 + 1];
                    file[out + 2] = row[x * 4 + 0];
                    file[out + 3] = row[x * 4 + 3];
                }
            }
        }

        std::ofstream output(std::filesystem::path(ICON_FILE), std::ios::binary);
        output.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
        output.close();
        LOGF_INFO("window icon taken from the title's logo: {} bytes", file.size());
        if (AR_ENV("AR_MAKE_ICON")) // --make-icon: that was all it came for. At once, as the title's threads are still running.
            TerminateProcess(GetCurrentProcess(), 0);
        ApplyGameIcon();
    }

    void ApplyGameIcon()
    {
        HWND window = FindWindowW(L"ARRecomp", nullptr);
        if (!window)
            return;
        HICON big = HICON(LoadImageW(nullptr, ICON_FILE, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
        HICON little = HICON(LoadImageW(nullptr, ICON_FILE, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
        if (big)
            PostMessageW(window, WM_SETICON, ICON_BIG, LPARAM(big));
        if (little)
            PostMessageW(window, WM_SETICON, ICON_SMALL, LPARAM(little));
    }
}
