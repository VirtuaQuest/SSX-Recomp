// Launcher: a window to choose display options, which then starts ARRecomp.exe with them. The choices (and the
// game image path) are kept in launcher.ini next to the launcher, with paths relative to it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "theme.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    struct Resolution
    {
        int width, height;
        const wchar_t* label;
    };

    constexpr Resolution RESOLUTIONS[] = {
        { 1280, 720, L"1280 x 720 (720p, native)" },
        { 1366, 768, L"1366 x 768" },
        { 1600, 900, L"1600 x 900" },
        { 1920, 1080, L"1920 x 1080 (1080p)" },
        { 2560, 1080, L"2560 x 1080 (ultrawide)" },
        { 2560, 1440, L"2560 x 1440 (1440p)" },
        { 3440, 1440, L"3440 x 1440 (ultrawide)" },
        { 3840, 2160, L"3840 x 2160 (4K)" },
    };

    constexpr const wchar_t* DISPLAY_MODES[] = { L"Windowed", L"Borderless fullscreen", L"Fullscreen" };
    constexpr const wchar_t* DISPLAY_ARGS[] = { L"windowed", L"borderless", L"fullscreen" };
    constexpr int FRAME_RATES[] = { 30, 60, 120 };
    constexpr const wchar_t* FRAME_RATE_LABELS[] = { L"30 FPS (original)", L"60 FPS (experimental)", L"120 FPS (experimental)" };
    constexpr const wchar_t* SCALE_LABELS[] = { L"1x (1280 x 720, original)", L"2x (2560 x 1440)", L"3x (3840 x 2160)", L"4x (5120 x 2880)" };
    constexpr const wchar_t* AA_LABELS[] = { L"Off", L"FXAA" };
    constexpr const wchar_t* ANISOTROPIC_LABELS[] = { L"As the game asks (original)", L"2x", L"4x", L"8x", L"16x" };
    constexpr const wchar_t* ANISOTROPIC_VALUES[] = { L"0", L"2", L"4", L"8", L"16" };
    constexpr const wchar_t* TEXTURE_LABELS[] = { L"Original", L"High", L"Ultra (may shimmer)" };
    constexpr const wchar_t* TEXTURE_VALUES[] = { L"0", L"-0.5", L"-1" };
    constexpr const wchar_t* DRAW_DISTANCE_LABELS[] = { L"Original", L"Far (2x)", L"Very far (4x)", L"Extreme (8x)", L"Always full detail" };
    constexpr const wchar_t* DRAW_DISTANCE_VALUES[] = { L"1", L"2", L"4", L"8", L"1000" };
    constexpr const wchar_t* SHARPEN_LABELS[] = { L"Off", L"Low", L"Medium", L"High" };
    constexpr const wchar_t* ULTRAWIDE_LABELS[] = { L"Off (16:9 with bars)", L"Wider view on ultrawide resolutions" };
    constexpr const wchar_t* VSYNC_LABELS[] = { L"Off", L"On" };
    constexpr const wchar_t* SHADOW_LABELS[] = { L"On", L"Off" };
    constexpr const wchar_t* MOUSE_LABELS[] = { L"Low", L"Medium", L"High", L"Very high" };
    constexpr const wchar_t* MOUSE_VALUES[] = { L"0.5", L"1", L"2", L"4" };
    constexpr const wchar_t* INVERT_LABELS[] = { L"Normal", L"Inverted" };
    constexpr const wchar_t* CONTROLLER_LABELS[] = { L"1", L"2", L"3", L"4" };
    constexpr const wchar_t* ONLINE_LABELS[] = { L"Off (original, no Xbox Live)", L"On (experimental: host and join)" };

    enum Control
    {
        ID_RESOLUTION = 100,
        ID_DISPLAY,
        ID_FPS,
        ID_AA,
        ID_ANISOTROPIC,
        ID_TEXTURES,
        ID_DRAW_DISTANCE,
        ID_SHARPEN,
        ID_ULTRAWIDE,
        ID_VSYNC,
        ID_SCALE,
        ID_SHADOWS,
        ID_MOUSE,
        ID_INVERT,
        ID_CONTROLLER,
        ID_ONLINE,
        ID_ONLINE_PEER,
        ID_PLAYER_NAME,
        ID_GAME_PATH,
        ID_BROWSE,
        ID_LAUNCH,
    };

    HWND g_resolution, g_display, g_fps, g_aa, g_anisotropic, g_textures, g_drawDistance, g_sharpen, g_ultrawide, g_vsync, g_scale, g_shadows, g_mouse, g_invert, g_gamePath, g_online, g_onlinePeer, g_playerName, g_controller;
    theme::Form g_form;
    std::wstring g_folder, g_iniPath;

    std::wstring ReadSetting(const wchar_t* key, const wchar_t* fallback)
    {
        wchar_t buffer[MAX_PATH * 2];
        GetPrivateProfileStringW(L"Launcher", key, fallback, buffer, DWORD(std::size(buffer)), g_iniPath.c_str());
        return buffer;
    }

    // Paths in launcher.ini are relative to the launcher's folder, so the file works wherever the folders are on a PC. A path on another drive can't be
    // relative and stays absolute. Absolute paths are still read, and saved as relative ones at the next launch.
    std::wstring ToAbsolute(const std::wstring& path)
    {
        return path.empty() ? path : (std::filesystem::path(g_folder) / path).lexically_normal().wstring();
    }

    std::wstring ToRelative(const std::wstring& path)
    {
        if (path.empty())
            return path;
        std::filesystem::path relative = std::filesystem::path(ToAbsolute(path)).lexically_relative(g_folder);
        return relative.empty() ? path : relative.wstring();
    }

    // The game keeps its saves and caches in "user" next to where it runs: the folder set in launcher.ini (WorkingFolder), or the launcher's own if
    // that isn't on this PC.
    std::wstring WorkingFolder()
    {
        std::wstring folder = ToAbsolute(ReadSetting(L"WorkingFolder", L"."));
        DWORD attributes = GetFileAttributesW(folder.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? folder : g_folder;
    }

    int ReadIndex(const wchar_t* key, int count, int fallback = 0)
    {
        int value = GetPrivateProfileIntW(L"Launcher", key, fallback, g_iniPath.c_str());
        return value >= 0 && value < count ? value : fallback;
    }

    void WriteSetting(const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(L"Launcher", key, value.c_str(), g_iniPath.c_str());
    }

    std::wstring GetText(HWND control)
    {
        std::wstring text(size_t(GetWindowTextLengthW(control)) + 1, L'\0');
        GetWindowTextW(control, text.data(), int(text.size()));
        text.resize(wcslen(text.c_str()));
        return text;
    }

    void Browse(HWND window)
    {
        wchar_t path[MAX_PATH * 2] = {};
        wcsncpy_s(path, ToAbsolute(GetText(g_gamePath)).c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = window;
        dialog.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = DWORD(std::size(path));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog))
            SetWindowTextW(g_gamePath, ToRelative(path).c_str());
    }

    void Launch(HWND window)
    {
        int resolution = int(SendMessageW(g_resolution, CB_GETCURSEL, 0, 0));
        int display = int(SendMessageW(g_display, CB_GETCURSEL, 0, 0));
        int fps = int(SendMessageW(g_fps, CB_GETCURSEL, 0, 0));
        int aa = int(SendMessageW(g_aa, CB_GETCURSEL, 0, 0));
        int anisotropic = int(SendMessageW(g_anisotropic, CB_GETCURSEL, 0, 0));
        int textures = int(SendMessageW(g_textures, CB_GETCURSEL, 0, 0));
        int drawDistance = int(SendMessageW(g_drawDistance, CB_GETCURSEL, 0, 0));
        int sharpen = int(SendMessageW(g_sharpen, CB_GETCURSEL, 0, 0));
        int ultrawide = int(SendMessageW(g_ultrawide, CB_GETCURSEL, 0, 0));
        int vsync = int(SendMessageW(g_vsync, CB_GETCURSEL, 0, 0));
        int scale = int(SendMessageW(g_scale, CB_GETCURSEL, 0, 0));
        int shadows = int(SendMessageW(g_shadows, CB_GETCURSEL, 0, 0));
        int mouse = int(SendMessageW(g_mouse, CB_GETCURSEL, 0, 0));
        int invert = int(SendMessageW(g_invert, CB_GETCURSEL, 0, 0));
        std::wstring gamePath = ToAbsolute(GetText(g_gamePath));
        int online = int(SendMessageW(g_online, CB_GETCURSEL, 0, 0));
        int controller = int(SendMessageW(g_controller, CB_GETCURSEL, 0, 0));
        std::wstring onlinePeer = GetText(g_onlinePeer), playerName = GetText(g_playerName);

        if (GetFileAttributesW(gamePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            MessageBoxW(window, L"Choose the Anarchy Reigns disc image (.iso) first.", L"Anarchy Reigns", MB_ICONWARNING);
            return;
        }

        WriteSetting(L"Resolution", std::to_wstring(resolution));
        WriteSetting(L"Display", std::to_wstring(display));
        WriteSetting(L"FrameRate", std::to_wstring(fps));
        WriteSetting(L"AntiAliasing", std::to_wstring(aa));
        WriteSetting(L"Anisotropic", std::to_wstring(anisotropic));
        WriteSetting(L"TextureDetail", std::to_wstring(textures));
        WriteSetting(L"DrawDistance", std::to_wstring(drawDistance));
        WriteSetting(L"Sharpen", std::to_wstring(sharpen));
        WriteSetting(L"Ultrawide", std::to_wstring(ultrawide));
        WriteSetting(L"VSync", std::to_wstring(vsync));
        WriteSetting(L"RenderScale", std::to_wstring(scale));
        WriteSetting(L"Shadows", std::to_wstring(shadows));
        WriteSetting(L"MouseSensitivity", std::to_wstring(mouse));
        WriteSetting(L"InvertMouse", std::to_wstring(invert));
        WriteSetting(L"Game", ToRelative(gamePath));
        WriteSetting(L"Online", std::to_wstring(online));
        WriteSetting(L"Controller", std::to_wstring(controller));
        WriteSetting(L"OnlinePeer", onlinePeer);
        WriteSetting(L"PlayerName", playerName);

        const Resolution& r = RESOLUTIONS[resolution];
        std::wstring exe = g_folder + L"\\ARRecomp.exe";
        std::wstring command = L"\"" + exe + L"\" --game \"" + gamePath + L"\" --resolution " + std::to_wstring(r.width) + L"x" +
            std::to_wstring(r.height) + L" --display " + DISPLAY_ARGS[display] + L" --fps " + std::to_wstring(FRAME_RATES[fps]) +
            L" --antialiasing " + std::to_wstring(aa) + L" --anisotropic " + ANISOTROPIC_VALUES[anisotropic] + L" --texture-lod-bias " + TEXTURE_VALUES[textures] +
            L" --draw-distance " + DRAW_DISTANCE_VALUES[drawDistance] + L" --sharpen " + std::to_wstring(sharpen) + (ultrawide == 1 ? L" --ultrawide" : L"") + (vsync == 1 ? L" --vsync" : L"") +
            L" --render-scale " + std::to_wstring(scale + 1) +
            L" --shadows " + (shadows == 0 ? L"on" : L"off") + L" --mouse-sensitivity " + MOUSE_VALUES[mouse] +
            (invert == 1 ? L" --invert-mouse" : L"");
        if (controller != 0)
            command += L" --controller " + std::to_wstring(controller + 1);
        if (online == 1)
        {
            command += L" --online";
            if (!onlinePeer.empty())
                command += L" --online-peer \"" + onlinePeer + L"\"";
            if (!playerName.empty())
                command += L" --player-name \"" + playerName + L"\"";
        }

        std::wstring workingFolder = WorkingFolder();
        if (workingFolder != g_folder) // Otherwise the setting is left alone: it may be a drive that's only away for now.
            WriteSetting(L"WorkingFolder", ToRelative(workingFolder));

        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, workingFolder.c_str(), &startup, &process))
        {
            MessageBoxW(window, (L"Could not start " + exe).c_str(), L"Anarchy Reigns", MB_ICONERROR);
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        DestroyWindow(window);
    }

    constexpr int WINDOW_WIDTH = 860, WINDOW_HEIGHT = 650;

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LRESULT handled;
        if (g_form.Handle(message, wParam, lParam, handled))
            return handled;

        switch (message)
        {
        case WM_CREATE:
        {
            std::vector<const wchar_t*> resolutionLabels;
            for (const auto& r : RESOLUTIONS)
                resolutionLabels.push_back(r.label);

            g_form.Attach(window, WINDOW_WIDTH, WINDOW_HEIGHT, L"Launcher", L"Anarchy Reigns Recompiled");

            constexpr int ROW = 36, SECTION_TOP = 30, LABEL_WIDTH = 132;
            constexpr int left = 24, right = 442, columnWidth = 394;
            int x = left, y = 96;
            auto section = [&](int rows, const wchar_t* title)
                {
                    g_form.AddSection(x, y, columnWidth, SECTION_TOP + rows * ROW + 4, title);
                    y += SECTION_TOP;
                };
            auto combo = [&](const wchar_t* label, int id, const wchar_t* const* items, size_t count, const wchar_t* key, int fallback = 0)
                {
                    g_form.AddLabel(x + 2, y, LABEL_WIDTH, 28, label);
                    HWND control = g_form.AddCombo(id, x + LABEL_WIDTH + 4, y, columnWidth - LABEL_WIDTH - 12, items, count, ReadIndex(key, int(count), fallback));
                    y += ROW;
                    return control;
                };
            auto edit = [&](const wchar_t* label, int id, const wchar_t* key)
                {
                    g_form.AddLabel(x + 2, y, LABEL_WIDTH, 28, label);
                    HWND control = g_form.AddEdit(id, x + LABEL_WIDTH + 4, y, columnWidth - LABEL_WIDTH - 12, ReadSetting(key, L""));
                    y += ROW;
                    return control;
                };

            section(6, L"Display");
            g_resolution = combo(L"Resolution", ID_RESOLUTION, resolutionLabels.data(), resolutionLabels.size(), L"Resolution");
            g_display = combo(L"Display mode", ID_DISPLAY, DISPLAY_MODES, std::size(DISPLAY_MODES), L"Display");
            g_fps = combo(L"Frame rate", ID_FPS, FRAME_RATE_LABELS, std::size(FRAME_RATE_LABELS), L"FrameRate");
            g_scale = combo(L"Internal resolution", ID_SCALE, SCALE_LABELS, std::size(SCALE_LABELS), L"RenderScale");
            g_vsync = combo(L"V-Sync", ID_VSYNC, VSYNC_LABELS, std::size(VSYNC_LABELS), L"VSync");
            g_ultrawide = combo(L"Ultrawide", ID_ULTRAWIDE, ULTRAWIDE_LABELS, std::size(ULTRAWIDE_LABELS), L"Ultrawide");
            y += 12;
            section(6, L"Graphics");
            g_aa = combo(L"Anti-aliasing", ID_AA, AA_LABELS, std::size(AA_LABELS), L"AntiAliasing");
            g_anisotropic = combo(L"Anisotropic filtering", ID_ANISOTROPIC, ANISOTROPIC_LABELS, std::size(ANISOTROPIC_LABELS), L"Anisotropic");
            g_textures = combo(L"Texture detail", ID_TEXTURES, TEXTURE_LABELS, std::size(TEXTURE_LABELS), L"TextureDetail");
            g_drawDistance = combo(L"Draw distance", ID_DRAW_DISTANCE, DRAW_DISTANCE_LABELS, std::size(DRAW_DISTANCE_LABELS), L"DrawDistance");
            g_sharpen = combo(L"Sharpening", ID_SHARPEN, SHARPEN_LABELS, std::size(SHARPEN_LABELS), L"Sharpen");
            g_shadows = combo(L"Shadows", ID_SHADOWS, SHADOW_LABELS, std::size(SHADOW_LABELS), L"Shadows");

            x = right;
            y = 96;
            section(3, L"Controls");
            g_controller = combo(L"Controller", ID_CONTROLLER, CONTROLLER_LABELS, std::size(CONTROLLER_LABELS), L"Controller");
            g_mouse = combo(L"Mouse sensitivity", ID_MOUSE, MOUSE_LABELS, std::size(MOUSE_LABELS), L"MouseSensitivity", 1);
            g_invert = combo(L"Mouse look", ID_INVERT, INVERT_LABELS, std::size(INVERT_LABELS), L"InvertMouse");
            y += 12;
            section(3, L"Multiplayer");
            g_online = combo(L"Multiplayer", ID_ONLINE, ONLINE_LABELS, std::size(ONLINE_LABELS), L"Online");
            g_playerName = edit(L"Player name", ID_PLAYER_NAME, L"PlayerName");
            g_onlinePeer = edit(L"Host's IP (to join)", ID_ONLINE_PEER, L"OnlinePeer");
            y += 12;
            section(1, L"Game");
            g_form.AddLabel(x + 2, y, 90, 28, L"Game image");
            g_gamePath = g_form.AddEdit(ID_GAME_PATH, x + 92, y, columnWidth - 92 - 86, ToRelative(ReadSetting(L"Game", L"")));
            g_form.AddButton(ID_BROWSE, x + columnWidth - 80, y, 74, 28, L"Browse");

            g_form.AddButton(ID_LAUNCH, x + columnWidth - 200, WINDOW_HEIGHT - 64, 200, 44, L"Launch", true);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_BROWSE)
                Browse(window);
            else if (LOWORD(wParam) == ID_LAUNCH || LOWORD(wParam) == IDOK)
                Launch(window);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_folder = exePath;
    g_folder.resize(g_folder.find_last_of(L'\\'));
    g_iniPath = g_folder + L"\\launcher.ini";

    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"ARLauncher";
    // The icon the game cut from its own logo the first time it ran, if it has. Otherwise a plain one.
    std::wstring iconPath = WorkingFolder() + L"\\user\\game_icon.ico";
    windowClass.hIcon = HICON(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
    windowClass.hIconSm = HICON(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
    // What setup stamped into this program, on a PC where the game hasn't run.
    if (windowClass.hIcon == nullptr)
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (windowClass.hIconSm == nullptr)
        windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (windowClass.hIcon == nullptr)
        windowClass.hIcon = theme::MakeIcon(GetSystemMetrics(SM_CXICON));
    if (windowClass.hIconSm == nullptr)
        windowClass.hIconSm = theme::MakeIcon(GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&windowClass);

    RECT rect{ 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Anarchy Reigns Launcher", style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}
