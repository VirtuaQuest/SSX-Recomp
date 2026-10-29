#pragma once

// The game window. It runs its own message loop thread; closing it ends the process.
namespace gpu::window
{
    // Creates the window (once), as g_videoSettings asks, and returns its handle.
    HWND Create(const char* title);

    // Client area size; changes when the user resizes the window.
    void GetClientSize(uint32_t& width, uint32_t& height);

    // Size of the swap chain's buffers: the client area, or the chosen resolution in borderless mode (stretched to
    // the monitor).
    void GetBufferSize(uint32_t& width, uint32_t& height);
}
