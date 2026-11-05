#include <stdafx.h>
#include "window.h"
#include "video_settings.h"
#include <kernel/keyboard_mouse.h>

namespace gpu
{
    VideoSettings g_videoSettings;
}

namespace gpu::window
{
    static HWND g_window;
    static std::atomic<uint32_t> g_width;
    static std::atomic<uint32_t> g_height;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LRESULT inputResult;
        if (kbm::HandleWindowMessage(hwnd, message, wParam, lParam, inputResult))
            return inputResult;

        switch (message)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                g_width = std::max<uint32_t>(LOWORD(lParam), 1);
                g_height = std::max<uint32_t>(HIWORD(lParam), 1);
            }
            return 0;

        case WM_CLOSE:
            LOGF_INFO("window closed");
            ExitProcess(0);

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    HWND Create(const char* title)
    {
        static std::once_flag once;
        std::call_once(once, [&]
            {
                std::mutex mutex;
                std::condition_variable created;
                std::string name = title;

                std::thread([&, name]
                    {
                        WNDCLASSEXW windowClass{ sizeof(windowClass) };
                        windowClass.lpfnWndProc = WindowProc;
                        windowClass.hInstance = GetModuleHandleW(nullptr);
                        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
                        windowClass.lpszClassName = L"ARRecomp";
                        // The window's and the taskbar's icon, once it has been taken from the game (game_icon.cpp).
                        windowClass.hIcon = HICON(LoadImageW(nullptr, L"user/game_icon.ico", IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
                        windowClass.hIconSm = HICON(LoadImageW(nullptr, L"user/game_icon.ico", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
                        if (windowClass.hIcon == nullptr)
                            windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(1));
                        if (windowClass.hIconSm == nullptr)
                            windowClass.hIconSm = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(1));
                        RegisterClassExW(&windowClass);

                        const auto& settings = g_videoSettings;
                        DWORD style = WS_OVERLAPPEDWINDOW;
                        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
                        RECT rect{ 0, 0, LONG(settings.width), LONG(settings.height) };

                        // The monitor the window opens on (the primary one).
                        MONITORINFO monitor{ sizeof(monitor) };
                        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);

                        if (settings.mode == DisplayMode::Borderless)
                        {
                            style = WS_POPUP;
                            rect = monitor.rcMonitor;
                            x = rect.left;
                            y = rect.top;
                        }
                        else if (settings.mode == DisplayMode::Fullscreen)
                        {
                            // DXGI switches the display mode once the swap chain exists.
                            style = WS_POPUP;
                            x = monitor.rcMonitor.left;
                            y = monitor.rcMonitor.top;
                        }
                        else
                        {
                            AdjustWindowRect(&rect, style, FALSE);
                            // Centered in the work area.
                            const RECT& work = monitor.rcWork;
                            x = std::max<LONG>(work.left, work.left + ((work.right - work.left) - (rect.right - rect.left)) / 2);
                            y = std::max<LONG>(work.top, work.top + ((work.bottom - work.top) - (rect.bottom - rect.top)) / 2);
                        }

                        std::wstring wideName(name.begin(), name.end());
                        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, wideName.c_str(), style, x, y,
                            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);

                        RECT client;
                        GetClientRect(hwnd, &client);
                        g_width = std::max<LONG>(client.right - client.left, 1);
                        g_height = std::max<LONG>(client.bottom - client.top, 1);
                        kbm::AttachWindow(hwnd);
                        // --make-icon runs unseen.
                        ShowWindow(hwnd, getenv("AR_MAKE_ICON") ? SW_HIDE : SW_SHOW);
                        {
                            std::lock_guard lock(mutex);
                            g_window = hwnd;
                        }
                        created.notify_one();

                        MSG message;
                        while (GetMessageW(&message, nullptr, 0, 0) > 0)
                        {
                            TranslateMessage(&message);
                            DispatchMessageW(&message);
                        }
                    }).detach();

                std::unique_lock lock(mutex);
                created.wait(lock, [] { return g_window != nullptr; });
            });

        return g_window;
    }

    void GetClientSize(uint32_t& width, uint32_t& height)
    {
        width = g_width;
        height = g_height;
    }

    void GetBufferSize(uint32_t& width, uint32_t& height)
    {
        if (g_videoSettings.mode == DisplayMode::Borderless)
        {
            width = g_videoSettings.width;
            height = g_videoSettings.height;
            return;
        }

        GetClientSize(width, height);
    }
}
