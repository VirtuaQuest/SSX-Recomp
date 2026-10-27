#include <stdafx.h>
#include "window.h"

namespace gpu::window
{
    static HWND g_window;
    static std::atomic<uint32_t> g_width;
    static std::atomic<uint32_t> g_height;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
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

    HWND Create(uint32_t width, uint32_t height, const char* title)
    {
        static std::once_flag once;
        std::call_once(once, [&]
            {
                std::mutex mutex;
                std::condition_variable created;
                std::string name = title;

                std::thread([&, width, height, name]
                    {
                        WNDCLASSEXW windowClass{ sizeof(windowClass) };
                        windowClass.lpfnWndProc = WindowProc;
                        windowClass.hInstance = GetModuleHandleW(nullptr);
                        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
                        windowClass.lpszClassName = L"ARRecomp";
                        RegisterClassExW(&windowClass);

                        RECT rect{ 0, 0, LONG(width), LONG(height) };
                        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

                        std::wstring wideName(name.begin(), name.end());
                        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, wideName.c_str(), WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                            windowClass.hInstance, nullptr);

                        g_width = width;
                        g_height = height;
                        ShowWindow(hwnd, SW_SHOW);
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
}
