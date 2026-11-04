#pragma once
#include <Xinput.h>
#include <gpu/d3d12_renderer.h>

// Keyboard and mouse on top of controller 0: they are read while the game's window is in front and merged into the
// state the title gets, so both work at the same time.
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

    // Whether the keyboard or mouse was used more recently than a controller (the UI then shows their icons).
    bool Active();

    // Whether the title is in a menu: some keys and mouse buttons mean something else there, and their icons follow.
    bool InMenu();

    // A menu row on screen, in 1280 x 720 coordinates: the UI's sprite draw reports them, one frame at a time.
    struct MenuRow
    {
        float x0, y0, x1, y1;
        bool selected;
        bool window = false; // In a list of the kind windows popping up have, between thin lines with a selector moving over them.
    };
    void SetMenuRows(const std::vector<MenuRow>& rows);

    // The prompts for breaking out of a stun: with the keyboard and mouse in use, the UI's sprite draw rearranges them.
    void SetStunPromptVisible(bool visible);
    bool StunPromptVisible();

    // The rows added to the Options screen. The title's update of that screen calls NoteOptionsUpdate, and plays the sounds asked
    // for after that (the ids its own menus use). The UI's draws lay out its rows with these (centres, in 1280 x 720 coordinates), and
    // the renderer draws what GetOverlay gives.
    void NoteOptionsUpdate();
    int TakeOptionsSound();
    bool OptionsVisible();
    bool OptionsLayoutActive();
    bool OptionsExtraSelected();
    int OptionsExtraRow();                 // Selected added row, or -1. In the order: 0 mouse camera, 1 mouse aiming camera, 2 and 3 the controls.
    int OptionsMouseLine();                // The line selected in an added row opened, or -1.
    int OptionsGameRow();
    int OptionsGameLine();
    bool OptionsGameOpen();                // One of the title's rows is opened, which makes the list longer.
    float OptionsGameRowY(int row);        // Where the title draws its row.
    float OptionsShift(float centerY);     // How much lower something it draws at this height goes.
    float OptionsExtraRowY(int row);
    float OptionsExtraHighlight(int row);  // How far the highlights have grown, and how far the title's may be while an added row is selected.
    float OptionsGameHighlightLimit();
    float OptionsOpenScale();
    void SetOptionsTextAlpha(float alpha);
    void GetOverlay(std::vector<gpu::d3d12::OverlayQuad>& quads);

    // The Gallery's character viewer, while it's up (the title's update tells): its own keys, and the mouse turns the character.
    bool ViewerActive();
    void NoteViewer();
    // What the mouse did since last asked, in radians to turn the character and tilt the camera. False when nothing.
    bool TakeViewerMouse(float& turn, float& tilt);

    // Testing (AR_KBM_TEST): the keys, buttons, cursor and mouse movement come from the remote control ("key <vk> <0 or 1>", "cursor <x> <y>" on the
    // 1280 x 720 picture, "mouse <dx> <dy>") instead of the real ones, whichever window is in front.
    void TestCommand(const std::string& command);

    // The title screen's prompt, which reads Press Any Button here, in the title's stencil letters, and is true of it: the text item
    // of the original gives where it goes, and isn't drawn. False when it can't be replaced.
    bool ReplaceTitlePrompt(float x, float y, float alpha);

    // The title sets its camera's settings from what it saves, the speeds last. While the mouse is turning the camera: sets them to the
    // mouse's settings instead, and gives the speed to use.
    bool MouseCameraSpeed(bool aiming, float& speed);

    // Mouse sensitivity (--mouse-sensitivity, 1 is the default) and looking up with the mouse going down.
    void SetMouseOptions(float sensitivity, bool invertY);
}
