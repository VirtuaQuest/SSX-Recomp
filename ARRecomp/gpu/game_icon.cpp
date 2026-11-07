// The window's icon is the title's own dashboard icon, read from the player's executable when it is loaded: the XEX has a
// resource named after the title ID holding its XDBF data, where image 0x8000 is the title's icon as a PNG. That PNG goes
// into user/game_icon.ico as it is (icon files may hold PNG images), so no art of the game is in this project or its
// executable; it only ever exists on the player's PC. AR_NO_GAME_ICON leaves it out.
#include <stdafx.h>
#include "window.h"
#include <kernel/memory.h>
#include <kernel/xex_loader.h>
#include <xex.h>
#include <xdbf_wrapper.h>

namespace gpu::window
{
    static constexpr const wchar_t* ICON_FILE = L"user/game_icon.ico";
    static constexpr uint64_t TITLE_ICON_ID = 0x8000;

    bool NeedsGameIcon()
    {
        static const bool disabled = AR_ENV("AR_NO_GAME_ICON") != nullptr;
        std::error_code ec;
        return !disabled && !std::filesystem::exists(ICON_FILE, ec);
    }

    // The title's XDBF block in the loaded image, from the XEX's resource list (8 byte names, address, size).
    static bool FindTitleData(const uint8_t*& data, uint32_t& size)
    {
        uint32_t field = FindXexHeaderField(g_module.xexHeader, XEX_HEADER_RESOURCE_INFO);
        if (field == 0)
            return false;
        uint32_t bytes = *g_memory.Translate<be<uint32_t>>(field);
        std::string titleName = fmt::format("{:08X}", g_module.titleId);
        for (uint32_t at = 4; at + 16 <= bytes; at += 16)
        {
            const char* name = g_memory.Translate<const char>(field + at);
            if (std::string_view(name, strnlen(name, 8)) != titleName)
                continue;
            uint32_t address = *g_memory.Translate<be<uint32_t>>(field + at + 8);
            size = *g_memory.Translate<be<uint32_t>>(field + at + 12);
            data = g_memory.Translate<const uint8_t>(address);
            return size != 0;
        }
        return false;
    }

    bool SaveTitleIcon()
    {
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!FindTitleData(data, size))
        {
            LOGF_WARNING("the title's icon wasn't found in its executable");
            return false;
        }
        XDBFWrapper xdbf(data, size);
        XDBFBlock png = xdbf.GetResource(XDBF_SPA_NAMESPACE_IMAGE, TITLE_ICON_ID);
        if (!png || png.BufferSize < 24 || memcmp(png.pBuffer, "\x89PNG", 4) != 0)
        {
            LOGF_WARNING("the title's executable has no icon image");
            return false;
        }

        // ICONDIR and one ICONDIRENTRY pointing at the PNG. Its size comes from the PNG's header (big endian, at 16 and 20).
        auto big32 = [&](size_t at) { return uint32_t(png.pBuffer[at]) << 24 | uint32_t(png.pBuffer[at + 1]) << 16 | uint32_t(png.pBuffer[at + 2]) << 8 | png.pBuffer[at + 3]; };
        uint32_t width = big32(16), height = big32(20);
        std::vector<uint8_t> file(6 + 16);
        auto put16 = [&](size_t at, uint32_t value) { file[at] = uint8_t(value); file[at + 1] = uint8_t(value >> 8); };
        auto put32 = [&](size_t at, uint32_t value) { put16(at, value & 0xFFFF); put16(at + 2, value >> 16); };
        put16(2, 1);
        put16(4, 1);
        file[6] = uint8_t(width >= 256 ? 0 : width);
        file[7] = uint8_t(height >= 256 ? 0 : height);
        put16(10, 1);
        put16(12, 32);
        put32(14, uint32_t(png.BufferSize));
        put32(18, 22);
        file.insert(file.end(), png.pBuffer, png.pBuffer + png.BufferSize);

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(ICON_FILE).parent_path(), ec);
        std::ofstream output(std::filesystem::path(ICON_FILE), std::ios::binary);
        output.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
        if (!output)
            return false;
        LOGF_INFO("window icon taken from the title's executable: {}x{}, {} bytes", width, height, file.size());
        return true;
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
