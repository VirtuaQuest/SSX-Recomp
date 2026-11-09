#pragma once
#include <Xinput.h>
#include <gpu/d3d12_renderer.h>

// Keyboard and mouse on top of controller 0: they are read while the game's window is in front and merged into the
// state the title gets, so both work at the same time. Each controller input can be given keys in
// user/key_bindings.ini; the mouse moves the right stick (SSX's trick stick) while it is captured.
namespace kbm
{
    // Whether this input is on (AR_NO_KBM turns it off; test runs driven by AR_REMOTE or AR_AUTO_PRESS only have it with AR_KBM).
    bool Enabled();

    // For the window's message loop. Returns true when the message was handled, with the result to return.
    bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

    // Called once the window exists, on its thread.
    void AttachWindow(HWND window);

    // Merges keyboard and mouse into controller 0's state. Notes which of the two was used last.
    void Apply(XINPUT_STATE& state, bool padConnected);

    // Whether the keyboard or mouse was used more recently than a controller.
    bool Active();

    // Title-specific prompt replacement isn't done for SSX: these say there is none.
    bool InMenu();
    bool StunPromptVisible();
    bool ViewerActive();

    // Quads the renderer draws over the picture (none at the moment).
    void GetOverlay(std::vector<gpu::d3d12::OverlayQuad>& quads);

    // Testing (AR_KBM_TEST): the keys, buttons and mouse movement come from the remote control ("key <vk> <0 or 1>",
    // "mouse <dx> <dy>") instead of the real ones, whichever window is in front.
    void TestCommand(const std::string& command);

    // The file system says when a movie file is open. Movies aren't skipped by the runtime for SSX (the title lets
    // its own be skipped), so MovieSkipping is always false.
    void NoteMovieOpen(bool open);
    bool MovieSkipping();

    // Mouse sensitivity (--mouse-sensitivity, 1 is the default) and the mouse going down pushing the stick up.
    void SetMouseOptions(float sensitivity, bool invertY);

    // Writes user/key_bindings.ini with the defaults if it doesn't exist (--write-key-bindings).
    void WriteDefaultBindings();
}
