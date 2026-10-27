#pragma once

// The game window. It runs its own message loop thread; closing it ends the process.
namespace gpu::window
{
    // Creates the window (once) and returns its handle.
    HWND Create(uint32_t width, uint32_t height, const char* title);

    // Client area size; changes when the user resizes the window.
    void GetClientSize(uint32_t& width, uint32_t& height);
}
