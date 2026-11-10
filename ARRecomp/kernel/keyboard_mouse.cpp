// Keyboard and mouse as a controller. Keys and mouse buttons are read when the title asks for controller state; mouse
// movement arrives as raw input on the window's thread and becomes the right stick, which is SSX's trick stick: a
// quick flick of the mouse is a flick of the stick, and it springs back when the mouse stops.
//
// Every controller input has keys of its own (user/key_bindings.ini, written with the defaults the first time), so
// the keyboard works the same in menus and on the mountain. The mouse is captured (cursor hidden and kept in the
// window) after clicking into the window, or whenever the window is in front in borderless and fullscreen modes.
// Alt or leaving the window releases it.
#include <stdafx.h>
#include "keyboard_mouse.h"
#include <kernel/memory.h>
#include <gpu/video_settings.h>

namespace kbm
{
    // Controller inputs a key can press. Stick directions are full deflections; triggers are pulled all the way.
    enum Input
    {
        IN_A, IN_B, IN_X, IN_Y, IN_LB, IN_RB, IN_LT, IN_RT, IN_START, IN_BACK, IN_LS_CLICK, IN_RS_CLICK,
        IN_DPAD_UP, IN_DPAD_DOWN, IN_DPAD_LEFT, IN_DPAD_RIGHT,
        IN_LS_UP, IN_LS_DOWN, IN_LS_LEFT, IN_LS_RIGHT,
        IN_RS_UP, IN_RS_DOWN, IN_RS_LEFT, IN_RS_RIGHT,
        INPUT_COUNT,
    };

    struct InputInfo
    {
        const char* name;   // In the bindings file.
        uint16_t button;    // XINPUT_GAMEPAD bits, or 0 for triggers and sticks.
        int keys[2];        // The defaults.
        const char* action; // What it does in SSX (the Standard controls), written above it in the bindings file.
    };

    // SSX's Standard controls (its manual, pages 3 and 4) laid out for one hand on WASD and one on the mouse: the
    // mouse is the trick stick (flick up to jump, flick to grab, circle to tweak), the left button boosts, the right
    // one grinds. Grabs also have keys around WASD (Q left, E right, F front, Space back), as the face buttons do.
    static constexpr InputInfo INPUTS[INPUT_COUNT] = {
        { "A", XINPUT_GAMEPAD_A, { VK_SPACE, VK_RETURN }, "Jump (hold to crouch, let go to jump); in the air, grab the back of the board; menus: select" },
        { "B", XINPUT_GAMEPAD_B, { 'E', VK_BACK }, "Trick; in the air, grab the right side of the board; menus: back" },
        { "X", XINPUT_GAMEPAD_X, { 'Q', 0 }, "Trick; in the air, grab the left side of the board" },
        { "Y", XINPUT_GAMEPAD_Y, { 'F', 0 }, "In the air, grab the front of the board" },
        { "LeftShoulder", XINPUT_GAMEPAD_LEFT_SHOULDER, { 'R', VK_PRIOR }, "Rewind (hold); menus: previous tab" },
        { "RightShoulder", XINPUT_GAMEPAD_RIGHT_SHOULDER, { VK_LCONTROL, VK_NEXT }, "Open the wingsuit (hold); menus: next tab" },
        { "LeftTrigger", 0, { VK_RBUTTON, 'C' }, "Grind; menus: previous page" },
        { "RightTrigger", 0, { VK_LBUTTON, VK_LSHIFT }, "Boost (hold); in the air, tweak the grab; menus: next page" },
        { "Start", XINPUT_GAMEPAD_START, { VK_ESCAPE, 0 }, "Pause" },
        { "Back", XINPUT_GAMEPAD_BACK, { VK_TAB, 0 }, "Back button" },
        { "LeftStickClick", XINPUT_GAMEPAD_LEFT_THUMB, { 'Z', 0 }, "Left stick click" },
        { "RightStickClick", XINPUT_GAMEPAD_RIGHT_THUMB, { VK_MBUTTON, 'V' }, "Right stick click" },
        { "DpadUp", XINPUT_GAMEPAD_DPAD_UP, { VK_UP, 0 }, "Menus: up" },
        { "DpadDown", XINPUT_GAMEPAD_DPAD_DOWN, { VK_DOWN, 0 }, "Menus: down" },
        { "DpadLeft", XINPUT_GAMEPAD_DPAD_LEFT, { VK_LEFT, 0 }, "Menus: left" },
        { "DpadRight", XINPUT_GAMEPAD_DPAD_RIGHT, { VK_RIGHT, 0 }, "Menus: right" },
        { "LeftStickUp", 0, { 'W', 0 }, "Lean forward; in the air, flip forward" },
        { "LeftStickDown", 0, { 'S', 0 }, "Lean back; in the air, flip backward" },
        { "LeftStickLeft", 0, { 'A', 0 }, "Steer left; in the air, spin left" },
        { "LeftStickRight", 0, { 'D', 0 }, "Steer right; in the air, spin right" },
        { "RightStickUp", 0, { 'I', VK_NUMPAD8 }, "Trick stick up (the mouse moves it too): jump; in the air, grab the front" },
        { "RightStickDown", 0, { 'K', VK_NUMPAD2 }, "Trick stick down: in the air, grab the back" },
        { "RightStickLeft", 0, { 'J', VK_NUMPAD4 }, "Trick stick left: in the air, grab the left side" },
        { "RightStickRight", 0, { 'L', VK_NUMPAD6 }, "Trick stick right: in the air, grab the right side" },
    };

    // The defaults before SSX had its own layout (the runtime's first title's): a bindings file with exactly these is
    // one nobody changed, and gets the layout above instead.
    static constexpr int OLD_DEFAULTS[INPUT_COUNT][2] = {
        { VK_SPACE, VK_RETURN }, { 'E', VK_BACK }, { VK_LSHIFT, 0 }, { 'Q', 0 }, { 'Z', 0 }, { 'C', 0 }, { VK_RBUTTON, '1' },
        { VK_LBUTTON, '3' }, { VK_ESCAPE, 0 }, { VK_TAB, 'R' }, { 'F', 0 }, { VK_MBUTTON, 'G' }, { VK_UP, 0 }, { VK_DOWN, 0 },
        { VK_LEFT, 0 }, { VK_RIGHT, 0 }, { 'W', 0 }, { 'S', 0 }, { 'A', 0 }, { 'D', 0 }, { 'I', VK_NUMPAD8 }, { 'K', VK_NUMPAD2 },
        { 'J', VK_NUMPAD4 }, { 'L', VK_NUMPAD6 },
    };

    static const char* KEY_BINDINGS_FILE = "user/key_bindings.ini";
    static int g_keys[INPUT_COUNT][2];

    // Key names for the bindings file: letters and digits as themselves, the rest by name.
    struct KeyName
    {
        int key;
        const char* name;
    };
    static constexpr KeyName KEY_NAMES[] = {
        { VK_LBUTTON, "MouseLeft" }, { VK_RBUTTON, "MouseRight" }, { VK_MBUTTON, "MouseMiddle" }, { VK_XBUTTON1, "Mouse4" }, { VK_XBUTTON2, "Mouse5" },
        { VK_SPACE, "Space" }, { VK_RETURN, "Enter" }, { VK_BACK, "Backspace" }, { VK_TAB, "Tab" }, { VK_ESCAPE, "Escape" },
        { VK_LSHIFT, "LeftShift" }, { VK_RSHIFT, "RightShift" }, { VK_LCONTROL, "LeftCtrl" }, { VK_RCONTROL, "RightCtrl" },
        { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
        { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" },
        { VK_NUMPAD0, "Num0" }, { VK_NUMPAD1, "Num1" }, { VK_NUMPAD2, "Num2" }, { VK_NUMPAD3, "Num3" }, { VK_NUMPAD4, "Num4" },
        { VK_NUMPAD5, "Num5" }, { VK_NUMPAD6, "Num6" }, { VK_NUMPAD7, "Num7" }, { VK_NUMPAD8, "Num8" }, { VK_NUMPAD9, "Num9" },
        { VK_OEM_1, "Semicolon" }, { VK_OEM_2, "Slash" }, { VK_OEM_3, "Backquote" }, { VK_OEM_4, "LeftBracket" }, { VK_OEM_5, "Backslash" },
        { VK_OEM_6, "RightBracket" }, { VK_OEM_7, "Quote" }, { VK_OEM_COMMA, "Comma" }, { VK_OEM_PERIOD, "Period" }, { VK_OEM_MINUS, "Minus" }, { VK_OEM_PLUS, "Equals" },
        { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" }, { VK_F5, "F5" }, { VK_F6, "F6" },
        { VK_F7, "F7" }, { VK_F8, "F8" }, { VK_F9, "F9" }, { VK_F11, "F11" }, { VK_F12, "F12" },
    };

    static std::string NameOfKey(int key)
    {
        if (key == 0)
            return "";
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            return std::string(1, char(key));
        for (const KeyName& name : KEY_NAMES)
            if (name.key == key)
                return name.name;
        return fmt::format("0x{:02X}", key);
    }

    static int KeyOfName(std::string name)
    {
        while (!name.empty() && isspace(uint8_t(name.back())))
            name.pop_back();
        while (!name.empty() && isspace(uint8_t(name.front())))
            name.erase(name.begin());
        if (name.empty() || name == "None")
            return 0;
        if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
            return name[0];
        if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
            return toupper(name[0]);
        for (const KeyName& entry : KEY_NAMES)
            if (_stricmp(entry.name, name.c_str()) == 0)
                return entry.key;
        // Other names for some keys, as Recomp Launcher's key binding editor writes them.
        static constexpr KeyName ALIASES[] = {
            { VK_RETURN, "Return" }, { VK_LSHIFT, "Shift" }, { VK_LCONTROL, "Ctrl" }, { VK_LCONTROL, "Control" },
            { VK_LBUTTON, "Mouse Left" }, { VK_RBUTTON, "Mouse Right" }, { VK_MBUTTON, "Mouse Middle" }, { VK_XBUTTON1, "Mouse 4" },
            { VK_XBUTTON2, "Mouse 5" },
        };
        for (const KeyName& entry : ALIASES)
            if (_stricmp(entry.name, name.c_str()) == 0)
                return entry.key;
        if (name.rfind("0x", 0) == 0)
            return std::clamp(int(strtol(name.c_str() + 2, nullptr, 16)), 0, 255);
        return -1;
    }

    static void SaveBindings()
    {
        std::ofstream file(KEY_BINDINGS_FILE);
        file << "; SSX keyboard and mouse layout: the keys for each controller input, up to two per line, separated by a comma.\n";
        file << "; Keyboard, mouse and controller all work at once; use whichever you like at any moment. The mouse moves the\n";
        file << "; right stick (the trick stick) while it's captured: click into the window; Alt lets it go.\n";
        file << "; Names: A-Z, 0-9, Space, Enter, Escape, Tab, Backspace, LeftShift, LeftCtrl, Up, Down, Left, Right, Num0-Num9,\n";
        file << "; MouseLeft, MouseRight, MouseMiddle, Mouse4, Mouse5, F1-F12 (not F10), Comma, Period, Slash, ... or None.\n";
        file << "; Delete this file to get the defaults back.\n";
        file << "[Keys]\n";
        for (int i = 0; i < INPUT_COUNT; i++)
        {
            file << "\n; " << INPUTS[i].action << '\n';
            file << INPUTS[i].name << " = " << NameOfKey(g_keys[i][0]);
            if (g_keys[i][1] != 0)
                file << ", " << NameOfKey(g_keys[i][1]);
            file << '\n';
        }
    }

    static void UseDefaults()
    {
        for (int i = 0; i < INPUT_COUNT; i++)
        {
            g_keys[i][0] = INPUTS[i].keys[0];
            g_keys[i][1] = INPUTS[i].keys[1];
        }
    }

    // Writes the default bindings file if there is none (--write-key-bindings, for the launcher).
    void WriteDefaultBindings()
    {
        if (std::filesystem::exists(KEY_BINDINGS_FILE))
            return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(KEY_BINDINGS_FILE).parent_path(), ec);
        UseDefaults();
        SaveBindings();
    }

    static void LoadBindings()
    {
        UseDefaults();
        std::ifstream file(KEY_BINDINGS_FILE);
        if (!file.is_open())
        {
            SaveBindings();
            return;
        }
        int loaded[INPUT_COUNT][2];
        memcpy(loaded, g_keys, sizeof(loaded));
        std::string line;
        while (std::getline(file, line))
        {
            if (size_t comment = line.find(';'); comment != std::string::npos)
                line.resize(comment);
            size_t equals = line.find('=');
            if (line.empty() || line[0] == '[' || equals == std::string::npos)
                continue;
            std::string name = line.substr(0, equals);
            name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return isspace(uint8_t(c)); }), name.end());
            int input = -1;
            for (int i = 0; i < INPUT_COUNT; i++)
                if (_stricmp(INPUTS[i].name, name.c_str()) == 0)
                    input = i;
            if (input < 0)
            {
                LOGF_WARNING("key bindings: no controller input called {}", name);
                continue;
            }
            std::string value = line.substr(equals + 1);
            size_t comma = value.find(',');
            int first = KeyOfName(value.substr(0, comma));
            int second = comma == std::string::npos ? 0 : KeyOfName(value.substr(comma + 1));
            if (first < 0 || second < 0)
            {
                LOGF_WARNING("key bindings: can't read the keys for {}: {}", name, value);
                continue;
            }
            loaded[input][0] = first;
            loaded[input][1] = second;
        }
        file.close();
        if (memcmp(loaded, OLD_DEFAULTS, sizeof(loaded)) == 0)
        {
            LOGF_INFO("key bindings: {} had the old defaults; it now has SSX's layout", KEY_BINDINGS_FILE);
            SaveBindings();
            return;
        }
        memcpy(g_keys, loaded, sizeof(loaded));
    }

    static const bool g_testInput = AR_ENV("AR_KBM_TEST") != nullptr;
    static std::atomic<bool> g_testKeys[256];

    static bool KeyIsDown(int key)
    {
        return g_testInput ? g_testKeys[key & 255].load() : (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    static bool IsMouseKey(int key)
    {
        return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2;
    }

    static HWND g_window;
    static std::atomic<bool> g_captured;
    static std::atomic<bool> g_ignoreLeftButton; // The click that captured the mouse isn't a press.
    static std::atomic<bool> g_clipped;          // The cursor is being kept in the window.
    static std::atomic<bool> g_moving;           // The player is dragging the window.
    static std::atomic<int32_t> g_mouseX, g_mouseY;
    static std::atomic<bool> g_active;
    static float g_sensitivity = 1.0f;
    static bool g_invertY;

    bool Enabled()
    {
        static const bool enabled = !AR_ENV("AR_NO_KBM") && ((!AR_ENV("AR_REMOTE") && !AR_ENV("AR_AUTO_PRESS")) || AR_ENV("AR_KBM"));
        return enabled;
    }

    void SetMouseOptions(float sensitivity, bool invertY)
    {
        g_sensitivity = sensitivity;
        g_invertY = invertY;
    }

    bool Active()
    {
        return g_active;
    }

    bool InMenu()
    {
        return false;
    }

    bool StunPromptVisible()
    {
        return false;
    }

    bool ViewerActive()
    {
        return false;
    }

    void GetOverlay(std::vector<gpu::d3d12::OverlayQuad>& quads)
    {
    }

    void NoteMovieOpen(bool open)
    {
    }

    bool MovieSkipping()
    {
        return false;
    }

    void TestCommand(const std::string& command)
    {
        std::istringstream words(command);
        std::string kind;
        words >> kind;
        if (kind == "key")
        {
            int key = 0, isDown = 0;
            words >> key >> isDown;
            g_testKeys[key & 255] = isDown != 0;
        }
        else if (kind == "mouse")
        {
            int dx = 0, dy = 0;
            words >> dx >> dy;
            g_mouseX += dx;
            g_mouseY += dy;
        }
    }

    static void ClipToWindow(HWND window)
    {
        RECT client;
        GetClientRect(window, &client);
        POINT topLeft{ client.left, client.top }, bottomRight{ client.right, client.bottom };
        ClientToScreen(window, &topLeft);
        ClientToScreen(window, &bottomRight);
        RECT area{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
        ClipCursor(&area);
    }

    static void UpdateClip(HWND window)
    {
        bool wanted = g_captured && !g_moving;
        if (wanted == g_clipped)
            return;
        g_clipped = wanted;
        if (wanted)
            ClipToWindow(window);
        else
            ClipCursor(nullptr);
    }

    static void SetCaptured(HWND window, bool captured)
    {
        g_captured = captured;
        UpdateClip(window);
        // Hides or shows the cursor (WM_SETCURSOR).
        POINT point;
        GetCursorPos(&point);
        SetCursorPos(point.x, point.y);
    }

    void AttachWindow(HWND window)
    {
        g_window = window;
        if (!Enabled())
            return;
        RAWINPUTDEVICE device{ 0x01, 0x02, 0, window }; // Generic desktop, mouse.
        if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
            LOGF_WARNING("no raw mouse input: {}", GetLastError());
    }

    bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if (!Enabled())
            return false;

        switch (message)
        {
        case WM_INPUT:
        {
            RAWINPUT input;
            UINT size = sizeof(input);
            if (g_captured && GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                input.header.dwType == RIM_TYPEMOUSE && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            {
                g_mouseX += input.data.mouse.lLastX;
                g_mouseY += input.data.mouse.lLastY;
            }
            return false; // DefWindowProc cleans up after WM_INPUT.
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE)
                SetCaptured(window, false);
            else if (gpu::g_videoSettings.mode != gpu::DisplayMode::Windowed)
                SetCaptured(window, true);
            return false;

        case WM_LBUTTONDOWN:
            if (!g_captured)
            {
                g_ignoreLeftButton = true;
                SetCaptured(window, true);
            }
            return false;

        case WM_NCLBUTTONDOWN:
            // On the title bar: not a click in the game, though the game's own check of the button would see it.
            g_ignoreLeftButton = true;
            return false;

        case WM_ENTERSIZEMOVE:
            g_moving = true;
            UpdateClip(window);
            return false;

        case WM_EXITSIZEMOVE:
            g_moving = false;
            UpdateClip(window);
            return false;

        case WM_SIZE:
        case WM_MOVE:
            if (g_clipped)
                ClipToWindow(window);
            return false;

        case WM_SETCURSOR:
            if (g_captured && LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(nullptr);
                result = TRUE;
                return true;
            }
            return false;

        case WM_SYSKEYDOWN:
            // Alt gives the mouse back; on its own it would also open the window's menu, as would F10.
            if (wParam == VK_MENU || wParam == VK_F10)
            {
                if (wParam == VK_MENU && g_captured)
                    SetCaptured(window, false);
                result = 0;
                return true;
            }
            return false;

        case WM_SYSCOMMAND:
            // Alt released on its own.
            if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0)
            {
                result = 0;
                return true;
            }
            return false;
        }
        return false;
    }

    // Mouse movement to the right stick. The counts since the last read are turned into a deflection that decays
    // quickly, so a flick reads as a flick and the stick returns to the middle when the mouse rests.
    static void MouseToStick(float& x, float& y)
    {
        static auto last = std::chrono::steady_clock::now();
        static float stickX, stickY;
        auto now = std::chrono::steady_clock::now();
        float seconds = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
        last = now;

        float dx = float(g_mouseX.exchange(0)), dy = float(g_mouseY.exchange(0));
        // About 40 counts in one read (a brisk flick on an 800 dpi mouse) push the stick all the way.
        static const float gain = (AR_ENV("AR_MOUSE_STICK_GAIN") ? float(atof(AR_ENV("AR_MOUSE_STICK_GAIN"))) : 1.0f) / 40.0f;
        float decay = std::exp(-seconds * 18.0f);
        stickX = std::clamp(stickX * decay + dx * gain * g_sensitivity, -1.0f, 1.0f);
        stickY = std::clamp(stickY * decay - dy * gain * g_sensitivity * (g_invertY ? -1.0f : 1.0f), -1.0f, 1.0f);
        if (std::abs(stickX) < 0.02f)
            stickX = 0.0f;
        if (std::abs(stickY) < 0.02f)
            stickY = 0.0f;
        x = stickX;
        y = stickY;
    }

    static SHORT CombineAxis(SHORT pad, float keys)
    {
        if (keys == 0.0f)
            return pad;
        return SHORT(std::clamp(int(pad) + int(keys * 32767.0f), -32768, 32767));
    }

    void Apply(XINPUT_STATE& state, bool padConnected)
    {
        auto& pad = state.Gamepad;
        // Without a controller, the keyboard and mouse count as in use from the start.
        static bool padSeen;
        padSeen |= padConnected;
        if (!padSeen && Enabled())
            g_active = true;
        bool padUsed = pad.wButtons != 0 || pad.bLeftTrigger > 30 || pad.bRightTrigger > 30 || std::abs(pad.sThumbLX) > 8000 || std::abs(pad.sThumbLY) > 8000 ||
            std::abs(pad.sThumbRX) > 8000 || std::abs(pad.sThumbRY) > 8000;
        if (padUsed)
            g_active = false;
        if (!Enabled())
            return;

        // Only while the window is in front: GetAsyncKeyState sees keys pressed anywhere.
        bool front = g_window != nullptr && (g_testInput || GetForegroundWindow() == g_window);
        if (g_testInput)
            g_captured = true;
        if (!front)
        {
            g_mouseX = 0;
            g_mouseY = 0;
            return;
        }
        if (!g_testInput)
            UpdateClip(g_window);

        static std::once_flag loaded;
        std::call_once(loaded, LoadBindings);

        if (g_ignoreLeftButton && !KeyIsDown(VK_LBUTTON))
            g_ignoreLeftButton = false;
        bool altDown = KeyIsDown(VK_MENU);

        bool on[INPUT_COUNT] = {};
        bool any = false;
        for (int i = 0; i < INPUT_COUNT; i++)
        {
            for (int key : g_keys[i])
            {
                if (key == 0 || altDown || !KeyIsDown(key))
                    continue;
                if (IsMouseKey(key) && (!g_captured || (key == VK_LBUTTON && g_ignoreLeftButton)))
                    continue;
                on[i] = true;
            }
            any |= on[i];
        }

        for (int i = 0; i < INPUT_COUNT; i++)
            if (on[i])
                pad.wButtons |= INPUTS[i].button;
        if (on[IN_LT])
            pad.bLeftTrigger = 255;
        if (on[IN_RT])
            pad.bRightTrigger = 255;

        float leftX = float(on[IN_LS_RIGHT]) - float(on[IN_LS_LEFT]);
        float leftY = float(on[IN_LS_UP]) - float(on[IN_LS_DOWN]);
        // Diagonals at full deflection, as a stick pushed into its corner gives.
        if (leftX != 0.0f && leftY != 0.0f)
        {
            leftX *= 0.7071f;
            leftY *= 0.7071f;
        }
        pad.sThumbLX = CombineAxis(pad.sThumbLX, leftX);
        pad.sThumbLY = CombineAxis(pad.sThumbLY, leftY);

        float rightX = float(on[IN_RS_RIGHT]) - float(on[IN_RS_LEFT]);
        float rightY = float(on[IN_RS_UP]) - float(on[IN_RS_DOWN]);
        float mouseX = 0.0f, mouseY = 0.0f;
        if (g_captured)
            MouseToStick(mouseX, mouseY);
        else
            g_mouseX = g_mouseY = 0;
        rightX = std::clamp(rightX + mouseX, -1.0f, 1.0f);
        rightY = std::clamp(rightY + mouseY, -1.0f, 1.0f);
        pad.sThumbRX = CombineAxis(pad.sThumbRX, rightX);
        pad.sThumbRY = CombineAxis(pad.sThumbRY, rightY);

        if (any || mouseX != 0.0f || mouseY != 0.0f)
            g_active = true;
    }
}
