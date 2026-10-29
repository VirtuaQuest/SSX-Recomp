// Launcher: a window to choose display options, which then starts ARRecomp.exe with them. The choices (and the
// game image path) are kept in launcher.ini next to the launcher.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
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
    constexpr const wchar_t* SCALE_LABELS[] = { L"1x (1280 x 720, original)", L"2x (2560 x 1440)", L"3x (3840 x 2160)" };
    constexpr const wchar_t* AA_LABELS[] = { L"Off", L"2x MSAA (not implemented yet)", L"4x MSAA (not implemented yet)" };

    enum Control
    {
        ID_RESOLUTION = 100,
        ID_DISPLAY,
        ID_FPS,
        ID_AA,
        ID_SCALE,
        ID_GAME_PATH,
        ID_BROWSE,
        ID_LAUNCH,
    };

    HWND g_resolution, g_display, g_fps, g_aa, g_scale, g_gamePath;
    HFONT g_font;
    std::wstring g_folder, g_iniPath;

    std::wstring ReadSetting(const wchar_t* key, const wchar_t* fallback)
    {
        wchar_t buffer[MAX_PATH * 2];
        GetPrivateProfileStringW(L"Launcher", key, fallback, buffer, DWORD(std::size(buffer)), g_iniPath.c_str());
        return buffer;
    }

    int ReadIndex(const wchar_t* key, int count)
    {
        int value = GetPrivateProfileIntW(L"Launcher", key, 0, g_iniPath.c_str());
        return value >= 0 && value < count ? value : 0;
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

    HWND AddLabel(HWND parent, const wchar_t* text, int x, int y, int width)
    {
        HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y + 4, width, 20, parent, nullptr, nullptr, nullptr);
        SendMessageW(label, WM_SETFONT, WPARAM(g_font), TRUE);
        return label;
    }

    HWND AddCombo(HWND parent, int id, int x, int y, int width, const wchar_t* const* items, size_t count, int selected)
    {
        HWND combo = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, width, 300, parent, HMENU(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(combo, WM_SETFONT, WPARAM(g_font), TRUE);
        for (size_t i = 0; i < count; i++)
            SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(items[i]));
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        return combo;
    }

    void Browse(HWND window)
    {
        wchar_t path[MAX_PATH * 2] = {};
        wcsncpy_s(path, GetText(g_gamePath).c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = window;
        dialog.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = DWORD(std::size(path));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog))
            SetWindowTextW(g_gamePath, path);
    }

    void Launch(HWND window)
    {
        int resolution = int(SendMessageW(g_resolution, CB_GETCURSEL, 0, 0));
        int display = int(SendMessageW(g_display, CB_GETCURSEL, 0, 0));
        int fps = int(SendMessageW(g_fps, CB_GETCURSEL, 0, 0));
        int aa = int(SendMessageW(g_aa, CB_GETCURSEL, 0, 0));
        int scale = int(SendMessageW(g_scale, CB_GETCURSEL, 0, 0));
        std::wstring gamePath = GetText(g_gamePath);

        if (GetFileAttributesW(gamePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            MessageBoxW(window, L"Choose the Anarchy Reigns disc image (.iso) first.", L"Anarchy Reigns", MB_ICONWARNING);
            return;
        }

        WriteSetting(L"Resolution", std::to_wstring(resolution));
        WriteSetting(L"Display", std::to_wstring(display));
        WriteSetting(L"FrameRate", std::to_wstring(fps));
        WriteSetting(L"AntiAliasing", std::to_wstring(aa));
        WriteSetting(L"RenderScale", std::to_wstring(scale));
        WriteSetting(L"Game", gamePath);

        const Resolution& r = RESOLUTIONS[resolution];
        std::wstring exe = g_folder + L"\\ARRecomp.exe";
        std::wstring command = L"\"" + exe + L"\" --game \"" + gamePath + L"\" --resolution " + std::to_wstring(r.width) + L"x" +
            std::to_wstring(r.height) + L" --display " + DISPLAY_ARGS[display] + L" --fps " + std::to_wstring(FRAME_RATES[fps]) +
            L" --antialiasing " + std::to_wstring(aa == 0 ? 0 : 1 << aa) + L" --render-scale " + std::to_wstring(scale + 1);

        // The game keeps its saves and caches in "user" next to where it runs: the folder set in launcher.ini
        // (WorkingFolder), or the launcher's own.
        std::wstring workingFolder = ReadSetting(L"WorkingFolder", g_folder.c_str());

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

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            std::vector<const wchar_t*> resolutionLabels;
            for (const auto& r : RESOLUTIONS)
                resolutionLabels.push_back(r.label);

            constexpr int labelX = 20, controlX = 150, width = 300;
            int y = 20;
            AddLabel(window, L"Resolution", labelX, y, 120);
            g_resolution = AddCombo(window, ID_RESOLUTION, controlX, y, width, resolutionLabels.data(), resolutionLabels.size(),
                ReadIndex(L"Resolution", int(std::size(RESOLUTIONS))));
            y += 40;
            AddLabel(window, L"Display mode", labelX, y, 120);
            g_display = AddCombo(window, ID_DISPLAY, controlX, y, width, DISPLAY_MODES, std::size(DISPLAY_MODES),
                ReadIndex(L"Display", int(std::size(DISPLAY_MODES))));
            y += 40;
            AddLabel(window, L"Frame rate", labelX, y, 120);
            g_fps = AddCombo(window, ID_FPS, controlX, y, width, FRAME_RATE_LABELS, std::size(FRAME_RATE_LABELS),
                ReadIndex(L"FrameRate", int(std::size(FRAME_RATES))));
            y += 40;
            AddLabel(window, L"Internal resolution", labelX, y, 125);
            g_scale = AddCombo(window, ID_SCALE, controlX, y, width, SCALE_LABELS, std::size(SCALE_LABELS),
                ReadIndex(L"RenderScale", int(std::size(SCALE_LABELS))));
            y += 40;
            AddLabel(window, L"Anti-aliasing", labelX, y, 120);
            g_aa = AddCombo(window, ID_AA, controlX, y, width, AA_LABELS, std::size(AA_LABELS), 0);
            EnableWindow(g_aa, FALSE);
            y += 50;

            AddLabel(window, L"Game image", labelX, y, 120);
            g_gamePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ReadSetting(L"Game", L"").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, controlX, y, width - 80, 24, window,
                HMENU(INT_PTR(ID_GAME_PATH)), nullptr, nullptr);
            SendMessageW(g_gamePath, WM_SETFONT, WPARAM(g_font), TRUE);
            HWND browse = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                controlX + width - 74, y, 74, 24, window, HMENU(INT_PTR(ID_BROWSE)), nullptr, nullptr);
            SendMessageW(browse, WM_SETFONT, WPARAM(g_font), TRUE);
            y += 50;

            HWND launch = CreateWindowExW(0, L"BUTTON", L"Launch", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                controlX + width - 120, y, 120, 32, window, HMENU(INT_PTR(ID_LAUNCH)), nullptr, nullptr);
            SendMessageW(launch, WM_SETFONT, WPARAM(g_font), TRUE);
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
    NONCLIENTMETRICSW metrics{ sizeof(metrics) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"ARLauncher";
    RegisterClassExW(&windowClass);

    RECT rect{ 0, 0, 480, 370 };
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
