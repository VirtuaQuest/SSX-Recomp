// Keyboard and mouse as a controller. Keys and mouse buttons are read when the title asks for controller state, mouse
// movement arrives as raw input on the window's thread and becomes the right stick (the title only has a stick for its
// camera, so very fast mouse movement saturates at the stick's turning speed).
//
// The mouse is captured (cursor hidden and kept in the window) after clicking into the window, or whenever the window is in
// front in borderless and fullscreen modes. Alt or leaving the window releases it.
//
// Some keys mean something else in menus. A key keeps the meaning it had when it went down: Esc opens the pause menu in
// gameplay and goes back in menus, and must not do both with one press. What the keys do in gameplay can be changed on the keyboard
// page of Options (saved in user/key_bindings.txt); some keys always do the same in menus.
#include <stdafx.h>
#include "keyboard_mouse.h"
#include <kernel/memory.h>
#include <gpu/video_settings.h>
#include <gpu/mkb_art_tables.inl>

namespace kbm
{
    static constexpr uint16_t TRIGGER_LEFT = 0x0400, TRIGGER_RIGHT = 0x0800; // Unused XINPUT_GAMEPAD bits.

    // What can be bound, each to a key or mouse button, as listed on the keyboard page.
    enum Action
    {
        ACT_MOVE_FORWARD, ACT_MOVE_BACK, ACT_MOVE_LEFT, ACT_MOVE_RIGHT, ACT_KILLER, ACT_STRONG_KILLER, ACT_WEAK_KILLER, ACT_LOCK_ON,
        ACT_TAUNT_1, ACT_TAUNT_2, ACT_ITEM_1, ACT_ITEM_2, ACT_RAMPAGE, ACT_MISSION, ACT_PAUSE,
        ACT_NORMAL, ACT_HEAVY, ACT_JUMP, ACT_GRAB, ACT_GUARD, ACT_EVADE, ACT_360,
        ACTION_COUNT,
    };

    struct ActionInfo
    {
        const char* name;
        uint16_t gameplay; // What it presses in gameplay and in menus. Moving is the left stick, not buttons.
        uint16_t menu;
        int key = 0;       // By default. Combinations have no key of their own unless given one.
        int secondKey = 0;
        uint16_t secondGameplay = 0;
    };

    static constexpr ActionInfo ACTIONS[ACTION_COUNT] = {
        { "Move Forward", 0, 0, 'W' },
        { "Move Back", 0, 0, 'S' },
        { "Move Left", 0, 0, 'A' },
        { "Move Right", 0, 0, 'D' },
        { "Use Killer Weapon", TRIGGER_LEFT, TRIGGER_LEFT, VK_RBUTTON, VK_LSHIFT, TRIGGER_LEFT },
        { "Strong Killer Weapon Attack", TRIGGER_LEFT | XINPUT_GAMEPAD_Y, 0, 'R' },
        { "Weak Killer Weapon Attack", TRIGGER_LEFT | XINPUT_GAMEPAD_X, 0 },
        { "Lock-On", XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER, VK_TAB, VK_MBUTTON, XINPUT_GAMEPAD_LEFT_SHOULDER },
        { "Taunt 1", XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP, 'T' },
        { "Taunt 2", XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN, 'V' },
        { "Use Item 1", XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT, '1' },
        { "Use Item 2", XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT, '2' },
        { "Unleash Rampage", XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB, 0, 'Q' }, // Both sticks at once.
        { "Start Mission", XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_BACK, 'F' },
        { "Pause", XINPUT_GAMEPAD_START, 0, VK_ESCAPE },
        { "Normal Attack", XINPUT_GAMEPAD_X, 0, VK_LBUTTON },
        { "Heavy Attack", XINPUT_GAMEPAD_Y, 0, VK_RBUTTON },
        { "Jump", XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_A, VK_SPACE },
        { "Grab", XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_B, 'E' },
        { "Guard", XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_LCONTROL, 'C', TRIGGER_RIGHT },
        { "Evade", XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_A, 0 },
        { "360 Attack", XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_A, 0 },
    };

    // Always the same in menus, whatever is bound.
    struct MenuKey
    {
        int key;
        uint16_t buttons;
    };
    static constexpr MenuKey MENU_KEYS[] = {
        { VK_RETURN, XINPUT_GAMEPAD_A }, { VK_BACK, XINPUT_GAMEPAD_B }, { VK_ESCAPE, XINPUT_GAMEPAD_B }, { 'R', XINPUT_GAMEPAD_Y }, { 'X', XINPUT_GAMEPAD_X },
        { VK_UP, XINPUT_GAMEPAD_DPAD_UP }, { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN }, { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
        { VK_LBUTTON, XINPUT_GAMEPAD_A }, { VK_RBUTTON, XINPUT_GAMEPAD_B },
    };

    static std::atomic<int> g_keys[ACTION_COUNT][2];
    static const char* KEY_BINDINGS_FILE = "user/key_bindings.txt";
    static constexpr int BINDINGS_VERSION = 2; // A last line after the bindings; files without it are from before right click readied the weapon.

    static void SetDefaultBindings()
    {
        for (int i = 0; i < ACTION_COUNT; i++)
        {
            g_keys[i][0] = ACTIONS[i].key;
            g_keys[i][1] = ACTIONS[i].secondKey;
        }
    }

    static void SaveBindings()
    {
        std::ofstream file(KEY_BINDINGS_FILE);
        for (int i = 0; i < ACTION_COUNT; i++)
            file << g_keys[i][0].load() << ' ' << g_keys[i][1].load() << '\n';
        file << BINDINGS_VERSION << " 0\n";
    }

    static void LoadBindings()
    {
        SetDefaultBindings();
        std::ifstream file(KEY_BINDINGS_FILE);
        int first, second;
        for (int i = 0; i < ACTION_COUNT && file >> first >> second; i++)
        {
            g_keys[i][0] = std::clamp(first, 0, 255);
            g_keys[i][1] = std::clamp(second, 0, 255);
        }
        int version = 0;
        if (!file.is_open() || (file >> version >> second && version >= 2))
            return;
        // Saved with the old defaults: the weapon moves to right click, SHIFT goes on working. Anything chosen by hand stays.
        auto isBound = [](int key)
            {
                for (int i = 0; i < ACTION_COUNT; i++)
                    if (g_keys[i][0] == key || g_keys[i][1] == key)
                        return true;
                return false;
            };
        if (g_keys[ACT_KILLER][0] == VK_LSHIFT && g_keys[ACT_KILLER][1] == 0 && g_keys[ACT_HEAVY][0] == VK_RBUTTON)
        {
            g_keys[ACT_KILLER][0] = VK_RBUTTON;
            g_keys[ACT_KILLER][1] = VK_LSHIFT;
        }
        if (g_keys[ACT_STRONG_KILLER][0] == 0 && !isBound('R'))
            g_keys[ACT_STRONG_KILLER][0] = 'R';
    }

    // Heavy Attack and the killer weapon can share a key: tapped it's the attack, held the weapon.
    static bool CanShareKey(int first, int second)
    {
        return (first == ACT_KILLER && second == ACT_HEAVY) || (first == ACT_HEAVY && second == ACT_KILLER);
    }

    static bool WeaponHeldOnAttackKey()
    {
        return g_keys[ACT_KILLER][0] != 0 && g_keys[ACT_KILLER][0] == g_keys[ACT_HEAVY][0];
    }

    // The action gets this key. An action that had it as its only one gets the key this one had, so nothing is left without a key
    // when it can be helped; a second key is just taken away.
    static void BindAction(int action, int key)
    {
        int previous = g_keys[action][0];
        for (int i = 0; i < ACTION_COUNT; i++)
        {
            if (i == action)
                continue;
            if (g_keys[i][1] == key)
                g_keys[i][1] = 0;
            if (g_keys[i][0] == key && !CanShareKey(i, action))
            {
                int second = g_keys[i][1];
                g_keys[i][0] = second != 0 ? second : previous;
                g_keys[i][1] = 0;
            }
        }
        g_keys[action][0] = key;
        g_keys[action][1] = 0;
    }

    static bool IsMouseKey(int key)
    {
        return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2;
    }

    static std::string KeyName(int key)
    {
        switch (key)
        {
        case 0: return "Not set";
        case VK_LBUTTON: return "Left click";
        case VK_RBUTTON: return "Right click";
        case VK_MBUTTON: return "Middle click";
        case VK_XBUTTON1: return "Mouse button 4";
        case VK_XBUTTON2: return "Mouse button 5";
        case VK_ESCAPE: return "ESC";
        case VK_SPACE: return "SPACE";
        case VK_TAB: return "TAB";
        case VK_RETURN: return "ENTER";
        case VK_BACK: return "BACKSPACE";
        case VK_LSHIFT: return "SHIFT";
        case VK_RSHIFT: return "RIGHT SHIFT";
        case VK_LCONTROL: return "CTRL";
        case VK_RCONTROL: return "RIGHT CTRL";
        case VK_UP: return "UP";
        case VK_DOWN: return "DOWN";
        case VK_LEFT: return "LEFT";
        case VK_RIGHT: return "RIGHT";
        case VK_CAPITAL: return "CAPS LOCK";
        case VK_APPS: return "MENU";
        }
        // What Windows calls it, on this keyboard's layout.
        UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        bool extended = (key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE || key == VK_NUMLOCK;
        char name[64] = {};
        if (scan != 0 && GetKeyNameTextA(LONG(scan << 16) | (extended ? 1 << 24 : 0), name, sizeof(name)) > 0)
        {
            for (char* c = name; *c; c++)
                *c = char(toupper(uint8_t(*c)));
            return name;
        }
        return fmt::format("KEY {}", key);
    }

    // How an action's binding reads.
    static std::string BindingText(int action)
    {
        int first = g_keys[action][0], second = g_keys[action][1];
        if (first != 0)
        {
            std::string text = KeyName(first);
            if (second != 0)
                text += " or " + KeyName(second);
            if (action == ACT_HEAVY && WeaponHeldOnAttackKey())
                return "Tap " + text;
            return action == ACT_KILLER ? "Hold " + text : text;
        }
        auto name = [](int other) { return KeyName(g_keys[other][0]); };
        switch (action)
        {
        case ACT_STRONG_KILLER: return name(ACT_KILLER) + " + " + name(ACT_HEAVY);
        case ACT_WEAK_KILLER: return name(ACT_KILLER) + " + " + name(ACT_NORMAL);
        case ACT_EVADE: return "Guard + " + name(ACT_JUMP);
        case ACT_360:
            if (g_keys[ACT_NORMAL][0] == VK_LBUTTON && g_keys[ACT_HEAVY][0] == VK_RBUTTON)
                return "Left + Right click";
            return name(ACT_NORMAL) + " + " + name(ACT_HEAVY);
        default: return "Not set";
        }
    }

    static const bool g_testInput = AR_ENV("AR_KBM_TEST") != nullptr;
    static std::atomic<bool> g_testKeys[256];
    static std::atomic<float> g_testCursorX{ 640.0f }, g_testCursorY{ 360.0f };

    static bool KeyIsDown(int key)
    {
        return g_testInput ? g_testKeys[key & 255].load() : (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    static HWND g_window;
    static std::atomic<bool> g_captured;
    static std::atomic<bool> g_ignoreLeftButton; // The click that captured the mouse isn't an attack.
    static std::atomic<bool> g_clipped;          // The cursor is being kept in the window.
    static std::atomic<bool> g_moving;           // The player is dragging the window.
    static std::atomic<int32_t> g_mouseX, g_mouseY, g_wheel;
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
        // AR_KBM_ICONS: always their icons (0: never), for testing the art.
        static const char* forced = AR_ENV("AR_KBM_ICONS");
        return forced ? atoi(forced) != 0 : g_active.load();
    }

    static std::atomic<bool> g_stunPrompt;

    void SetStunPromptVisible(bool visible)
    {
        // AR_LOG_BUTTONS also logs the stun prompts coming and going, to catch them in tests.
        static const bool logIt = AR_ENV("AR_LOG_BUTTONS") != nullptr;
        if (logIt && visible != g_stunPrompt.load())
            LOGF_INFO("stun prompt {}", visible ? "on" : "off");
        g_stunPrompt = visible;
    }

    bool StunPromptVisible()
    {
        return g_stunPrompt;
    }

    static int64_t Milliseconds();
    static std::atomic<int64_t> g_windowTime{ -1000 }; // When a window popping up was last seen drawn.

    static bool InMenuByFlags()
    {
        // The title's state flags: bit 0x02000000 on the title screen and main menus, 0x00200000 in the pause menu and what
        // it opens. Neither during gameplay, cutscenes and loading. (AR_TEST_IGNORE_MENU_FLAGS: for testing what follows on any menu.)
        static const bool ignore = AR_ENV("AR_TEST_IGNORE_MENU_FLAGS") != nullptr;
        if (g_memory.base == nullptr || ignore)
            return false;
        uint32_t flags = *reinterpret_cast<const be<uint32_t>*>(g_memory.Translate(0x83214CF0));
        return (flags & 0x02200000) != 0;
    }

    bool InMenu()
    {
        // Windows that pop up during gameplay, like the one for something unlocked, have none of those flags.
        return InMenuByFlags() || Milliseconds() - g_windowTime.load() < 250;
    }

    static std::atomic<int64_t> g_viewerTime{ -1000 };
    static std::atomic<int32_t> g_viewerMouseX, g_viewerMouseY;

    bool ViewerActive()
    {
        return Milliseconds() - g_viewerTime.load() < 200;
    }

    void NoteViewer()
    {
        g_viewerTime = Milliseconds();
    }

    int GetOptionsSelectedRow();

    // The Options screen with its added rows, in the order shown:
    //   the title's rows 0-3, Mouse Camera, Mouse Aiming Camera, the rest of the title's rows, Controller Controls, Keyboard And Mouse Controls.
    // The title has seven rows in its pause menu's Options and nine in the main menu's, drawn the same way by different code, and knows
    // nothing of the others, seeing no input on them. Everything of the title's below its row 3 is drawn lower (draw_hooks.cpp), the
    // added rows' backgrounds and highlights are its own sprites drawn again, their text is drawn over the picture (GetOverlay). Its
    // selection is read from the highlights it draws; while an added row is selected it waits on a row next to it, with its highlight
    // hidden, and moves by presses like any other.
    enum ExtraRow { ROW_MOUSE_CAMERA, ROW_MOUSE_AIMING, ROW_CONTROLS_PAD, ROW_CONTROLS_KEYBOARD, EXTRA_ROWS };
    static constexpr float OPTIONS_X = 88.0f, OPTIONS_WIDTH = 416.0f, OPTIONS_FIRST_Y = 193.0f, OPTIONS_STEP = 28.0f, OPTIONS_FIRST_CENTER = 207.0f;
    static constexpr float OPEN_WIDTH_SCALE = 1.24f, OPEN_HEIGHT = 56.0f; // A camera row opened to change its values: wider, two more lines.
    static constexpr int MIN_OPTIONS_ROWS = 7;
    static std::atomic<int> g_gameRows{ MIN_OPTIONS_ROWS }, g_gameRow, g_gameLine;

    static int GameRowDisplay(int row)
    {
        return row <= 3 ? row : row + 2;
    }

    static int ExtraRowDisplay(int row)
    {
        return row <= ROW_MOUSE_AIMING ? 4 + row : g_gameRows + row;
    }

    static int DisplayRows()
    {
        return g_gameRows + EXTRA_ROWS;
    }

    static std::atomic<bool> g_optionsVisible;  // On screen, with nothing popped up over it: takes input.
    static std::atomic<bool> g_optionsLayout;   // On screen: its rows are laid out with the added ones.
    static std::atomic<int64_t> g_optionsMenuTime;
    static std::atomic<int> g_extraRow{ -1 };   // Selected added row.
    static std::atomic<int> g_controlsPage{ 0 };  // 1 controller, 2 keyboard and mouse.
    static std::atomic<int> g_editField{ -1 };    // The mouse row opened: the line selected.
    static std::atomic<float> g_extraScale[EXTRA_ROWS], g_gameHighlightLimit{ 1.0f }, g_openScale{ 1.0f }, g_optionsTextAlpha{ 1.0f };
    static std::atomic<uint32_t> g_optionsSounds;
    static std::atomic<int> g_gameOpenLines;    // Lines more than its seven rows the title's list has: a row of it opened.

    // Each camera has its own values, as the controller's do: invert vertical, invert horizontal, speed 1-10.
    struct MouseCamera
    {
        std::atomic<bool> invertY, invertX;
        std::atomic<int> speed{ 5 };
    };
    static MouseCamera g_mouseCamera[2]; // Normal, aiming.
    static bool g_editBackupY, g_editBackupX;
    static int g_editBackupSpeed;

    static const char* MOUSE_SETTINGS_FILE = "user/mouse_settings.txt";

    static void LoadMouseSettings()
    {
        std::ifstream file(MOUSE_SETTINGS_FILE);
        int values[6] = { 0, 0, 5, 0, 0, 5 };
        int count = 0;
        while (count < 6 && file >> values[count])
            count++;
        for (int i = 0; i < 2; i++)
        {
            g_mouseCamera[i].invertY = values[i * 3] != 0;
            g_mouseCamera[i].invertX = values[i * 3 + 1] != 0;
            g_mouseCamera[i].speed = std::clamp(values[i * 3 + 2], 1, 10);
        }
    }

    static void SaveMouseSettings()
    {
        std::ofstream file(MOUSE_SETTINGS_FILE);
        for (const MouseCamera& camera : g_mouseCamera)
            file << (camera.invertY ? 1 : 0) << ' ' << (camera.invertX ? 1 : 0) << ' ' << camera.speed.load() << ' ';
        file << '\n';
    }

    static uint32_t GuestWord(uint32_t address)
    {
        return *reinterpret_cast<const be<uint32_t>*>(g_memory.Translate(address));
    }

    static int64_t Milliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The row selected, and in a row opened its line selected, as last drawn.
    int OptionsGameRow()
    {
        return g_gameRow;
    }

    int OptionsGameLine()
    {
        return g_gameLine;
    }

    bool OptionsGameOpen()
    {
        return g_gameOpenLines > 0;
    }

    static bool OptionsGameEditing()
    {
        return g_gameOpenLines > 0;
    }

    int OptionsExtraRow()
    {
        return g_extraRow;
    }

    int OptionsMouseLine()
    {
        return g_editField;
    }

    bool OptionsLayoutActive()
    {
        return g_optionsLayout;
    }

    // The centre of a row of the title's, where it draws it.
    float OptionsGameRowY(int row)
    {
        bool open = OptionsGameOpen() && row > OptionsGameRow();
        return OPTIONS_FIRST_CENTER + OPTIONS_STEP * float(row + (open ? g_gameOpenLines.load() : 0));
    }

    float OptionsShift(float centerY)
    {
        if (centerY < OptionsGameRowY(4) - OPTIONS_STEP * 0.5f || centerY >= 600.0f)
            return 0.0f;
        return OPTIONS_STEP * 2.0f + (g_editField >= 0 ? OPEN_HEIGHT : 0.0f);
    }

    float OptionsExtraRowY(int row)
    {
        bool mouseOpen = g_editField >= 0;
        switch (row)
        {
        case ROW_MOUSE_CAMERA:
            return OptionsGameRowY(4);
        case ROW_MOUSE_AIMING:
            return OptionsGameRowY(4) + OPTIONS_STEP + (mouseOpen && g_extraRow == ROW_MOUSE_CAMERA ? OPEN_HEIGHT : 0.0f);
        default:
        {
            float last = OptionsGameRowY(g_gameRows - 1);
            return last + OptionsShift(last) + OPTIONS_STEP * float(row - ROW_CONTROLS_PAD + 1);
        }
        }
    }

    static float DisplayRowY(int display)
    {
        for (int i = 0; i < g_gameRows; i++)
        {
            if (GameRowDisplay(i) == display)
                return OptionsGameRowY(i) + OptionsShift(OptionsGameRowY(i));
        }
        for (int i = 0; i < EXTRA_ROWS; i++)
        {
            if (ExtraRowDisplay(i) == display)
                return OptionsExtraRowY(i);
        }
        return 0.0f;
    }

    float OptionsExtraHighlight(int row)
    {
        return g_extraScale[row];
    }

    float OptionsGameHighlightLimit()
    {
        return g_gameHighlightLimit;
    }

    float OptionsOpenScale()
    {
        return g_openScale;
    }

    void SetOptionsTextAlpha(float alpha)
    {
        g_optionsTextAlpha = alpha;
    }

    // The sounds of the title's own menus: moving (544), changing a value (545), confirming (542), going back (543).
    static void PlayOptionsSound(int id)
    {
        g_optionsSounds |= 1u << (id - 542);
    }

    int TakeOptionsSound()
    {
        uint32_t sounds = g_optionsSounds.load();
        for (int bit = 0; bit < 4; bit++)
        {
            if (sounds & (1u << bit))
            {
                g_optionsSounds &= ~(1u << bit);
                return 542 + bit;
            }
        }
        return -1;
    }

    void NoteOptionsUpdate()
    {
        int64_t now = Milliseconds();
        static int64_t last = now;
        float seconds = std::min(float(now - last) / 1000.0f, 0.1f);
        last = now;
        g_optionsMenuTime = now;
        static const float speed = AR_ENV("AR_OPTIONS_HIGHLIGHT_SPEED") ? float(atof(AR_ENV("AR_OPTIONS_HIGHLIGHT_SPEED"))) : 4.4f;
        auto approach = [&](std::atomic<float>& value, float target, float rate)
            {
                float current = value;
                float step = rate * seconds;
                value = current < target ? std::min(current + step, target) : std::max(current - step, target);
            };
        int extra = g_extraRow;
        for (int i = 0; i < EXTRA_ROWS; i++)
            approach(g_extraScale[i], extra == i ? 1.0f : 0.0f, speed);
        approach(g_gameHighlightLimit, extra >= 0 ? 0.0f : 1.0f, speed);
        approach(g_openScale, g_editField >= 0 ? OPEN_WIDTH_SCALE : 1.0f, speed);
    }

    bool OptionsVisible()
    {
        return g_optionsVisible;
    }

    bool OptionsExtraSelected()
    {
        return g_extraRow >= 0;
    }

    // Text in the title's own letters, from the textures it uses for this screen. Only what the rows need has been measured, in
    // texels: left, top, right, bottom, and the line the letter stands on. Labels and values are different fonts.
    struct Glyph
    {
        char letter;
        float x0, y0, x1, y1, base;
    };
    static constexpr Glyph LETTERS[] = {
        { 'R', 123, 10, 134, 27, 27 }, { 'K', 141, 10, 152, 27, 27 }, { 'L', 159, 10, 169, 27, 27 }, { 'B', 176, 10, 186, 27, 27 }, { 'x', 193, 14, 203, 27, 27 },
        { 'g', 243, 14, 252, 31, 27 }, { 'M', 17, 45, 33, 62, 62 }, { 'w', 40, 49, 55, 62, 62 }, { 'm', 62, 49, 76, 62, 62 }, { 'V', 83, 45, 96, 62, 62 },
        { 'A', 103, 45, 116, 62, 62 }, { 'C', 123, 44, 134, 63, 62 }, { 'D', 141, 45, 152, 62, 62 }, { 'y', 159, 49, 169, 67, 62 }, { 'H', 176, 45, 186, 62, 62 },
        { 'u', 193, 49, 203, 63, 62 }, { 'b', 210, 45, 220, 63, 62 }, { 'p', 227, 49, 236, 66, 62 }, { 'd', 243, 45, 252, 62, 62 }, { 'O', 4, 80, 16, 99, 98 },
        { 'N', 23, 81, 35, 98, 98 }, { 'Y', 42, 81, 54, 98, 98 }, { 'X', 61, 81, 73, 98, 98 }, { 'G', 80, 80, 92, 99, 98 }, { 'T', 99, 81, 110, 98, 98 },
        { 'S', 117, 80, 128, 99, 98 }, { 'U', 152, 81, 162, 99, 98 }, { 'a', 169, 85, 179, 99, 98 }, { 'v', 186, 85, 196, 98, 98 }, { 'E', 203, 81, 212, 98, 98 },
        { 'J', 219, 81, 228, 99, 98 }, { 'I', 251, 81, 253, 98, 98 }, { 'k', 4, 116, 12, 133, 133 }, { 'o', 20, 120, 29, 133, 133 }, { 'F', 36, 116, 44, 133, 133 },
        { 'h', 51, 116, 59, 133, 133 }, { 'n', 66, 120, 74, 133, 133 }, { 'e', 81, 120, 89, 133, 133 }, { 'z', 96, 120, 104, 133, 133 }, { 'c', 111, 120, 119, 133, 133 },
        { 's', 126, 120, 134, 133, 133 }, { 'r', 141, 120, 147, 133, 133 }, { 'f', 154, 116, 160, 133, 133 }, { 't', 167, 116, 173, 133, 133 }, { '-', 180, 125, 186, 127, 133 },
        { 'j', 193, 116, 197, 137, 133 }, { 'l', 204, 116, 206, 133, 133 }, { '.', 214, 130, 216, 133, 133 }, { 'i', 223, 116, 225, 133, 133 }, { ',', 232, 130, 234, 136, 133 },
    };
    static constexpr Glyph VALUES[] = {
        { '0', 1994, 685, 2003, 702, 702 }, { '1', 1253, 313, 1260, 330, 330 }, { '2', 1916, 865, 1925, 882, 882 }, { '3', 2024, 577, 2033, 594, 594 }, { '4', 1898, 699, 1909, 717, 717 },
        { '5', 2009, 613, 2018, 630, 630 }, { '6', 1994, 649, 2003, 666, 666 }, { '7', 1933, 757, 1943, 774, 774 }, { '8', 1979, 685, 1988, 702, 702 }, { '9', 1964, 721, 1973, 738, 738 },
        { 'N', 1980, 571, 1990, 588, 588 }, { 'I', 1186, 19, 1190, 36, 36 },
    };
    static constexpr float TEXT_BASELINE = 8.5f, TEXT_GAP = 1.15f, TEXT_SPACE = 7.0f, DESCRIPTION_GAP = 1.88f;

    template <size_t N>
    static const Glyph* FindGlyph(const Glyph (&glyphs)[N], char letter)
    {
        for (const Glyph& glyph : glyphs)
        {
            if (glyph.letter == letter)
                return &glyph;
        }
        return nullptr;
    }

    struct TextStyle
    {
        bool value = false;      // The values' font.
        float align = 0.0f;      // 0 from x on, 0.5 around it, 1 up to it.
        float gap = TEXT_GAP;
        float maxWidth = 0.0f;   // Narrowed if it would be wider.
    };

    static void AddGameText(std::vector<gpu::d3d12::OverlayQuad>& quads, float x, float centerY, const char* text, const float* color, const TextStyle& style = {})
    {
        auto find = [&](char c) { return style.value ? FindGlyph(VALUES, c) : FindGlyph(LETTERS, c); };
        float total = 0.0f;
        for (const char* c = text; *c; c++)
        {
            const Glyph* glyph = find(*c);
            total += *c == ' ' ? TEXT_SPACE : glyph ? glyph->x1 - glyph->x0 + (c[1] ? style.gap : 0.0f) : 0.0f;
        }
        float narrow = style.maxWidth > 0.0f && total > style.maxWidth ? style.maxWidth / total : 1.0f;
        x -= total * narrow * style.align;
        float baseline = centerY + TEXT_BASELINE;
        for (const char* c = text; *c; c++)
        {
            if (*c == ' ')
            {
                x += TEXT_SPACE * narrow;
                continue;
            }
            const Glyph* glyph = find(*c);
            if (!glyph)
                continue;
            float width = (glyph->x1 - glyph->x0) * narrow, height = glyph->y1 - glyph->y0;
            // On whole pixels, or the texture's filtering blurs the letters. (Narrowed ones are a little blurred anyway.)
            float left = narrow == 1.0f ? std::floor(x + 0.5f) : x;
            quads.push_back({ left, baseline + glyph->y0 - glyph->base, width, height, style.value ? -5 : -4, { color[0], color[1], color[2], color[3] },
                { glyph->x0, glyph->y0, glyph->x1, glyph->y1 } });
            x += width + style.gap * narrow;
        }
    }

    // The title screen's prompt. The stencil letters on that screen's texture are only those of its own words; N, Y, U and O come from
    // the softer version of them used for big titles, given hard edges. Left, top, right, bottom in texels.
    struct StencilLetter
    {
        char letter;
        float x0, y0, x1, y1;
        bool soft;
    };
    static constexpr StencilLetter STENCIL_LETTERS[] = {
        { 'P', 214, 39, 247, 86, false }, { 'R', 301, 39, 334, 86, false }, { 'E', 369, 167, 405, 214, false }, { 'S', 115, 168, 154, 216, false }, { 'A', 199, 167, 236, 215, false },
        { 'B', 386, 38, 425, 85, false }, { 'T', 105, 295, 137, 344, false }, { 'N', 643, 507, 673, 554, true }, { 'Y', 1046, 120, 1077, 167, true }, { 'U', 1227, 888, 1256, 929, true },
        { 'O', 381, 507, 412, 553, true },
    };
    static std::atomic<float> g_titleX, g_titleY, g_titleAlpha;
    static std::atomic<int64_t> g_titleTime{ -1000 };

    bool ReplaceTitlePrompt(float x, float y, float alpha)
    {
        if (!Enabled() || !gpu::d3d12::TitleFontsAvailable())
            return false;
        g_titleX = x;
        g_titleY = y;
        g_titleAlpha = alpha;
        g_titleTime = Milliseconds();
        return true;
    }

    static bool TitlePromptShowing()
    {
        return Milliseconds() - g_titleTime.load() < 200;
    }

    static void AddTitlePrompt(std::vector<gpu::d3d12::OverlayQuad>& quads)
    {
        static constexpr float LETTER_HEIGHT = 49.0f, GAP = 7.0f, SPACE = 20.0f, WIDTH = 405.0f; // Short of the torn end of the strip.
        static const char* const TEXT = "PRESS ANY BUTTON";
        auto find = [](char c) -> const StencilLetter*
            {
                for (const StencilLetter& letter : STENCIL_LETTERS)
                {
                    if (letter.letter == c)
                        return &letter;
                }
                return nullptr;
            };
        auto naturalWidth = [](const StencilLetter& letter)
            {
                return (letter.x1 - letter.x0) * (letter.soft ? LETTER_HEIGHT / (letter.y1 - letter.y0) : 1.0f);
            };
        float total = 0.0f;
        for (const char* c = TEXT; *c; c++)
        {
            const StencilLetter* letter = find(*c);
            total += letter ? naturalWidth(*letter) + (c[1] ? GAP : 0.0f) : SPACE;
        }
        float scale = std::min(WIDTH / total, 1.0f);
        // Where the original's words are, from the place it draws them, on the same line through their middle.
        float x = g_titleX + 25.0f, bottom = g_titleY - 2.5f + LETTER_HEIGHT * scale * 0.5f;
        float alpha = g_titleAlpha;
        for (const char* c = TEXT; *c; c++)
        {
            const StencilLetter* letter = find(*c);
            if (!letter)
            {
                x += SPACE * scale;
                continue;
            }
            float width = naturalWidth(*letter) * scale;
            float height = (letter->soft ? LETTER_HEIGHT : letter->y1 - letter->y0) * scale;
            gpu::d3d12::OverlayQuad quad{ x, bottom - height, width, height, letter->soft ? -5 : -9, { 0.0f, 0.0f, 0.0f, alpha },
                { letter->x0, letter->y0, letter->x1, letter->y1 } };
            quad.sharpen = letter->soft;
            quads.push_back(quad);
            x += width + GAP * scale;
        }
    }

    // The keyboard page: three columns of a row for each action, and two that aren't keys to choose.
    static constexpr int PAGE_FIXED_CAMERA = -2, PAGE_FIXED_STUN = -3;
    static constexpr int PAGE_COLUMNS = 3, PAGE_ROWS = 8;
    static constexpr int PAGE_LAYOUT[PAGE_COLUMNS][PAGE_ROWS] = {
        { ACT_MOVE_FORWARD, ACT_MOVE_BACK, ACT_MOVE_LEFT, ACT_MOVE_RIGHT, ACT_KILLER, ACT_STRONG_KILLER, ACT_WEAK_KILLER, ACT_LOCK_ON },
        { ACT_TAUNT_1, ACT_TAUNT_2, ACT_ITEM_1, ACT_ITEM_2, ACT_RAMPAGE, ACT_MISSION, ACT_PAUSE, PAGE_FIXED_CAMERA },
        { ACT_NORMAL, ACT_HEAVY, ACT_JUMP, ACT_GRAB, ACT_GUARD, ACT_EVADE, ACT_360, PAGE_FIXED_STUN },
    };
    static constexpr float PAGE_COLUMN_X[PAGE_COLUMNS] = { 56.0f, 440.0f, 824.0f }, PAGE_COLUMN_WIDTH = 372.0f, PAGE_TOP = 438.0f, PAGE_STEP = 30.0f;
    static std::atomic<int> g_pageColumn, g_pageRow;
    static std::atomic<bool> g_pageWaitingForKey;

    static void AddPageText(std::vector<gpu::d3d12::OverlayQuad>& quads, float x, float top, const std::string& text, int weight, const float* color, bool alignRight)
    {
        float total = 0.0f;
        for (char c : text)
            total += (uint8_t(c) >= 32 && uint8_t(c) <= 126) ? float(g_pageFontCells[weight][c - 32][4]) / 16.0f : 0.0f;
        if (alignRight)
            x -= total;
        for (char c : text)
        {
            if (uint8_t(c) < 32 || uint8_t(c) > 126)
                continue;
            const auto& cell = g_pageFontCells[weight][c - 32];
            gpu::d3d12::OverlayQuad quad{ std::floor(x + 0.5f) - 2.0f, top, float(cell[2]), float(cell[3]), -8, { color[0], color[1], color[2], color[3] },
                { float(cell[0]), float(cell[1]), float(cell[0] + cell[2]), float(cell[1] + cell[3]) } };
            quads.push_back(quad);
            x += float(cell[4]) / 16.0f;
        }
    }

    static void GetKeyboardPage(std::vector<gpu::d3d12::OverlayQuad>& quads)
    {
        static constexpr float ERASE[4] = { 0.0f, 0.03f, 0.0f, 1.0f }, BRIGHT[4] = { 0.70f, 1.0f, 0.2f, 1.0f },
            LABEL[4] = { 0.70f, 1.0f, 0.2f, 0.75f }, ON_SELECTED[4] = { 0.02f, 0.05f, 0.0f, 1.0f };
        auto solid = [&](float x, float y, float width, float height, const float* color)
            {
                quads.push_back({ x, y, width, height, -1, { color[0], color[1], color[2], color[3] } });
            };
        solid(0.0f, 0.0f, 1280.0f, 720.0f, ERASE);
        quads.push_back({ 0.0f, 0.0f, 1280.0f, 720.0f, -2, { BRIGHT[0], BRIGHT[1], BRIGHT[2], BRIGHT[3] } });

        // Only the keys in use are lit.
        for (const auto& key : g_pageKeys)
        {
            bool used = false;
            for (int i = 0; i < ACTION_COUNT; i++)
                used |= g_keys[i][0] == key[0] || g_keys[i][1] == key[0];
            if (!used)
                continue;
            float x0 = key[1], y0 = key[2], x1 = key[3], y1 = key[4];
            solid(x0, y0, x1 - x0, y1 - y0, ERASE);
            quads.push_back({ x0, y0, x1 - x0, y1 - y0, -7, { BRIGHT[0], BRIGHT[1], BRIGHT[2], BRIGHT[3] }, { x0, y0, x1, y1 } });
        }

        int selectedColumn = g_pageColumn, selectedRow = g_pageRow;
        bool waiting = g_pageWaitingForKey;
        for (int column = 0; column < PAGE_COLUMNS; column++)
        {
            for (int row = 0; row < PAGE_ROWS; row++)
            {
                int action = PAGE_LAYOUT[column][row];
                float x = PAGE_COLUMN_X[column], y = PAGE_TOP + PAGE_STEP * float(row);
                bool selected = column == selectedColumn && row == selectedRow;
                if (selected)
                    solid(x, y, PAGE_COLUMN_WIDTH, PAGE_STEP - 2.0f, BRIGHT);
                std::string name, binding;
                if (action >= 0)
                {
                    name = ACTIONS[action].name;
                    binding = selected && waiting ? "Press a key to map" : BindingText(action);
                }
                else if (action == PAGE_FIXED_CAMERA)
                {
                    name = "Move Camera";
                    binding = "Mouse";
                }
                else
                {
                    name = "Break Out Of A Stun";
                    binding = "Mash movement + clicks";
                }
                AddPageText(quads, x + 8.0f, y, name, 0, selected ? ON_SELECTED : LABEL, false);
                AddPageText(quads, x + PAGE_COLUMN_WIDTH - 8.0f, y, binding, 1, selected ? ON_SELECTED : BRIGHT, true);
            }
        }
    }

    static void AddMovieOverlay(std::vector<gpu::d3d12::OverlayQuad>& quads);

    void GetOverlay(std::vector<gpu::d3d12::OverlayQuad>& quads)
    {
        static constexpr float LABEL_X = 97.5f, LONGEST_LABEL = 260.0f; // The values on the camera rows start after that.
        // The overlay's own text, if the title's fonts can't be found (see mkb_art.inl).
        static constexpr int TEXT_MOUSE_AIMING = 0, TEXT_PAD = 1, TEXT_KEYBOARD = 2, TEXT_CONTROLLER_AIMING = 3, TEXT_N = 4, TEXT_Y = 5, TEXT_DIGITS = 6;
        static constexpr float BRIGHT[4] = { 0.70f, 1.0f, 0.2f, 1.0f };

        if (TitlePromptShowing())
            AddTitlePrompt(quads);
        AddMovieOverlay(quads);

        int page = g_controlsPage;
        if (page == 2)
        {
            GetKeyboardPage(quads);
            return;
        }
        if (page != 0)
        {
            static constexpr float DIM[4] = { 0.0f, 0.03f, 0.0f, 0.98f };
            quads.push_back({ 0.0f, 0.0f, 1280.0f, 720.0f, -1, { DIM[0], DIM[1], DIM[2], DIM[3] } });
            // (The renderer shows the title's own diagram for the controller, once it has a picture of it.)
            quads.push_back({ 0.0f, 0.0f, 1280.0f, 720.0f, -3, { BRIGHT[0], BRIGHT[1], BRIGHT[2], BRIGHT[3] } });
            // How to leave, where the title's screens have their legend, over what's left of the one behind it.
            static constexpr float LEGEND[4] = { 0.878f, 0.878f, 0.878f, 1.0f };
            quads.push_back({ 0.0f, 636.0f, 1280.0f, 44.0f, -1, { 0.0f, 0.0f, 0.0f, 1.0f } });
            if (gpu::d3d12::GameFontsAvailable())
                AddGameText(quads, 98.0f, 657.5f, Active() ? "RIGHT CLICK TO CLOSE" : "B TO CLOSE", LEGEND);
            else
                AddPageText(quads, 98.0f, 644.0f, Active() ? "Right click to close" : "B to close", 1, LEGEND, false);
            return;
        }
        if (!g_optionsLayout)
            return;

        float alpha = g_optionsTextAlpha;
        const float GREY[4] = { 0.878f, 0.878f, 0.878f, alpha }, DIMMED[4] = { 0.878f, 0.878f, 0.878f, alpha * 0.4f }, BLACK[4] = { 0.039f, 0.039f, 0.039f, alpha },
            GREEN[4] = { 0.463f, 0.882f, 0.0f, alpha }, DARK[4] = { 0.078f, 0.078f, 0.078f, alpha };
        bool gameFonts = gpu::d3d12::GameFontsAvailable(), gameIcons = gpu::d3d12::GameIconsAvailable();

        int extra = g_extraRow, line = g_editField, gameRow = OptionsGameRow(), gameLine = OptionsGameLine();
        bool gameOpen = OptionsGameOpen(), mouseOpen = line >= 0, anyOpen = gameOpen || mouseOpen;

        auto rowLabel = [&](float center, const char* text, int fallback, const float* color, float fallbackWidth, float maxWidth = 0.0f)
            {
                if (gameFonts)
                {
                    TextStyle style;
                    style.maxWidth = maxWidth;
                    AddGameText(quads, LABEL_X, center - 0.5f, text, color, style);
                }
                else if (fallback >= 0)
                    quads.push_back({ LABEL_X, center - 11.0f, fallbackWidth, 27.0f, fallback, { color[0], color[1], color[2], color[3] } });
            };
        auto value = [&](float centerX, float center, const char* text, const float* color)
            {
                if (gameFonts)
                {
                    TextStyle style;
                    style.value = true;
                    style.align = 0.5f;
                    AddGameText(quads, centerX, center - 0.5f, text, color, style);
                }
            };
        // From the title's texture with its small icons: left, top, right, bottom in texels.
        auto icon = [&](float x, float y, const float* texels, const float* color)
            {
                if (gameIcons)
                {
                    quads.push_back({ x, y, std::abs(texels[2] - texels[0]), texels[3] - texels[1], -6, { color[0], color[1], color[2], color[3] },
                        { texels[0], texels[1], texels[2], texels[3] } });
                }
            };
        static constexpr float TILE_Y[4] = { 45, 1, 65, 21 }, TILE_X[4] = { 23, 23, 43, 43 }, TILE_SPEED[4] = { 67, 1, 87, 21 };
        static constexpr float TILE_MUSIC[4] = { 1, 1, 21, 21 }, TILE_EFFECTS[4] = { 23, 1, 43, 21 }, TILE_VOICES[4] = { 45, 23, 65, 43 };
        static constexpr float ARROW_LEFT[4] = { 79, 23, 67, 38 }, ARROW_RIGHT[4] = { 67, 23, 79, 38 }, ARROW_DOT[4] = { 89, 1, 94, 6 }; // Left: the right one, mirrored.

        // The cameras' labels, the title's new names, on its rows.
        auto gameLabelColor = [&](int row)
            {
                if (mouseOpen || (gameOpen && gameRow != row))
                    return DIMMED;
                return extra < 0 && gameRow == row && (!gameOpen || gameLine == 0) ? BLACK : GREY;
            };
        rowLabel(OptionsGameRowY(2), "CONTROLLER CAMERA", TEXT_CONTROLLER_AIMING, gameLabelColor(2), 195.0f);
        rowLabel(OptionsGameRowY(3), "CONTROLLER AIMING CAMERA", TEXT_CONTROLLER_AIMING, gameLabelColor(3), 195.0f, LONGEST_LABEL);

        // The icons of a row of the title's whose black selected ones aren't drawn while an added row is selected (draw_hooks.cpp).
        if (extra >= 0 && !anyOpen && gameRow >= 1 && gameRow <= 3)
        {
            float y = OptionsGameRowY(gameRow) - 10.0f;
            icon(383.0f, y, gameRow == 1 ? TILE_MUSIC : TILE_Y, GREEN);
            icon(430.0f, y, gameRow == 1 ? TILE_EFFECTS : TILE_X, GREEN);
            icon(477.0f, y, gameRow == 1 ? TILE_VOICES : TILE_SPEED, GREEN);
        }

        static const char* const EXTRA_NAMES[EXTRA_ROWS] = { "MOUSE CAMERA", "MOUSE AIMING CAMERA", "CONTROLLER CONTROLS", "KEYBOARD AND MOUSE CONTROLS" };
        static constexpr int FALLBACKS[EXTRA_ROWS] = { TEXT_MOUSE_AIMING, TEXT_MOUSE_AIMING, TEXT_PAD, TEXT_KEYBOARD };
        static constexpr float FALLBACK_WIDTHS[EXTRA_ROWS] = { 160.0f, 160.0f, 170.0f, 220.0f };
        for (int i = 0; i < EXTRA_ROWS; i++)
        {
            float center = OptionsExtraRowY(i);
            const float* color = anyOpen ? (mouseOpen && extra == i ? (line == 0 ? BLACK : GREY) : DIMMED) : (extra == i ? BLACK : GREY);
            rowLabel(center, EXTRA_NAMES[i], FALLBACKS[i], color, FALLBACK_WIDTHS[i]);
            if (i > ROW_MOUSE_AIMING || anyOpen)
                continue;
            // Closed, a camera row shows its three values and their icons.
            const MouseCamera& camera = g_mouseCamera[i];
            const float* text = extra == i ? BLACK : GREY;
            const float* tile = extra == i ? BLACK : GREEN;
            char speed[8];
            snprintf(speed, sizeof(speed), "%d", camera.speed.load());
            value(369.0f, center, camera.invertY ? "I" : "N", text);
            icon(383.0f, center - 10.0f, TILE_Y, tile);
            value(416.0f, center, camera.invertX ? "I" : "N", text);
            icon(430.0f, center - 10.0f, TILE_X, tile);
            value(463.0f, center, speed, text);
            icon(477.0f, center - 10.0f, TILE_SPEED, tile);
        }

        // Opened, as the title's do: a line for each value, wider, the line selected with arrows.
        if (mouseOpen && extra >= 0 && extra <= ROW_MOUSE_AIMING && gameFonts)
        {
            const MouseCamera& camera = g_mouseCamera[extra];
            float top = OptionsExtraRowY(extra);
            char speed[8];
            snprintf(speed, sizeof(speed), "%d", camera.speed.load());
            const char* const lines[3] = { camera.invertY ? "Inverted" : "Normal", camera.invertX ? "Inverted" : "Normal", "Movement Speed" };
            const char* const values[3] = { camera.invertY ? "I" : "N", camera.invertX ? "I" : "N", speed };
            const float* const tiles[3] = { TILE_Y, TILE_X, TILE_SPEED };
            TextStyle right;
            right.align = 1.0f;
            for (int i = 0; i < 3; i++)
            {
                float center = top + OPTIONS_STEP * float(i);
                bool selected = line == i;
                AddGameText(quads, 496.0f, center - 0.5f, lines[i], selected ? BLACK : GREY, right);
                value(563.0f, center, values[i], selected ? BLACK : GREY);
                icon(578.0f, center - 10.0f, tiles[i], selected ? BLACK : GREEN);
                if (selected)
                {
                    icon(510.0f, center - 8.0f, ARROW_LEFT, DARK);
                    icon(524.5f, center - 3.0f, ARROW_DOT, DARK);
                    icon(531.0f, center - 8.0f, ARROW_RIGHT, DARK);
                }
            }
        }

        // The description of an added row.
        if (extra >= 0 && gameFonts)
        {
            static const char* const DESCRIPTIONS[EXTRA_ROWS][2] = {
                { "Change Y and X-Axis behavior for the mouse", "camera, and adjust camera movement speed." },
                { "Change Y and X-Axis behavior for the mouse aiming", "camera, and adjust camera movement speed." },
                { "View the controls for the controller.", "" },
                { "View the controls for the keyboard and mouse.", "" },
            };
            TextStyle style;
            style.align = 1.0f;
            style.gap = DESCRIPTION_GAP;
            bool oneLine = DESCRIPTIONS[extra][1][0] == 0;
            AddGameText(quads, 1184.0f, oneLine ? 582.0f : 568.5f, DESCRIPTIONS[extra][0], GREY, style);
            AddGameText(quads, 1184.0f, 596.5f, DESCRIPTIONS[extra][1], GREY, style);
        }
    }

    static std::mutex g_rowsMutex;
    static std::vector<MenuRow> g_rows;
    static std::chrono::steady_clock::time_point g_rowsTime;
    static MenuGrid g_grid;
    static std::chrono::steady_clock::time_point g_gridTime;

    void SetMenuGrid(const MenuGrid& grid)
    {
        g_windowTime = Milliseconds(); // A menu, whatever the title's flags say.
        std::lock_guard lock(g_rowsMutex);
        g_grid = grid;
        g_gridTime = std::chrono::steady_clock::now();
    }

    static std::atomic<int> g_optionsSelected{ -1 };

    int GetOptionsSelectedRow()
    {
        return g_optionsSelected;
    }

    void SetMenuRows(const std::vector<MenuRow>& rows)
    {
        // A window popping up, its selection showing.
        bool windowShowing = false;
        for (const MenuRow& row : rows)
            windowShowing |= (row.window && row.selected) || row.alwaysMenu;
        if (windowShowing)
            g_windowTime = Milliseconds();
        // AR_LOG_OPTIONS: when they come and go, and the state the title is in then.
        static const bool logWindows = AR_ENV("AR_LOG_OPTIONS") != nullptr;
        static bool windowBefore;
        if (logWindows && windowShowing != windowBefore)
        {
            windowBefore = windowShowing;
            LOGF_INFO("menu window {} rows {} flags {:08X}", windowShowing ? "shows" : "gone", rows.size(), GuestWord(0x83214CF0));
            for (const MenuRow& row : rows)
                LOGF_INFO("  row {} {} {} {} selected {} window {}", row.x0, row.y0, row.x1, row.y1, row.selected, row.window);
        }

        // The Options screen: the title updates it, and its rows are at these places, more with one opened.
        static std::vector<std::pair<float, bool>> list;
        list.clear();
        for (const MenuRow& row : rows)
        {
            if (std::abs(row.x0 - OPTIONS_X) < 2.0f && row.y0 >= OPTIONS_FIRST_Y - 2.0f && std::abs(row.y1 - row.y0 - OPTIONS_STEP) < 2.0f)
                list.push_back({ row.y0, row.selected });
        }
        std::sort(list.begin(), list.end());
        int count = int(list.size());
        bool layout = Enabled() && Milliseconds() - g_optionsMenuTime.load() < 300 && count >= MIN_OPTIONS_ROWS;
        static bool layoutBefore;
        // Screens open with no row opened, so the fewest seen is how many rows it has.
        if (layout)
            g_gameRows = layoutBefore ? std::min(g_gameRows.load(), count) : count;
        layoutBefore = layout;
        g_optionsLayout = layout;
        int lines = layout ? count - g_gameRows : 0;
        g_gameOpenLines = lines;
        g_optionsVisible = layout && rows.size() == size_t(count);
        if (layout)
        {
            int selected = -1;
            for (int i = 0; i < count; i++)
                selected = list[i].second ? i : selected;
            if (selected >= 0 && lines == 0)
                g_gameRow = selected;
            else if (selected >= 0)
                g_gameLine = std::clamp(selected - g_gameRow, 0, lines);
            if (lines == 0)
                g_gameLine = 0;
        }
        else
        {
            if (g_extraRow >= 0)
            {
                g_extraRow = -1;
                g_gameHighlightLimit = 1.0f;
            }
            g_editField = -1;
        }

        std::lock_guard lock(g_rowsMutex);
        g_rows = rows;
        g_rowsTime = std::chrono::steady_clock::now();
    }

    // The mouse in menus: the row under the cursor gets selected, by pressing up or down until it is, since the title's menus
    // only know those. Rows are what the UI drew lately; a group of rows sharing an x position is one list, and the one with
    // a selected row is the one in use.
    struct MenuHover
    {
        bool hasRows = false; // A list is on screen.
        int selected = -1, target = -1; // In the list; target is under the cursor (-1: none).
        bool options = false;  // The Options screen's rows, in the order shown, or the lines of a row opened.
        int closeRow = -1;     // A window the title takes no Back on: the row that Back means. No when it asks to confirm, the only row otherwise.
        int arrow = 0;         // On the selected line of a row opened: over the arrow that lowers (-1) or raises (1) its value.
        int columns = 0;       // A grid: selected and target count along its rows, this many to a row.
        uint16_t pageButton = 0; // Over an arrow turning a grid's pages: what a click presses.
    };

    // The picture is 1280 x 720 scaled to fit the window, keeping its shape.
    static bool CursorOnPicture(HWND window, float& x, float& y)
    {
        if (g_testInput)
        {
            x = g_testCursorX;
            y = g_testCursorY;
            return true;
        }
        POINT cursor;
        RECT client;
        if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor) || !GetClientRect(window, &client))
            return false;
        float scale = std::min(float(client.right) / 1280.0f, float(client.bottom) / 720.0f);
        x = (float(cursor.x) - (float(client.right) - 1280.0f * scale) * 0.5f) / scale;
        y = (float(cursor.y) - (float(client.bottom) - 720.0f * scale) * 0.5f) / scale;
        return true;
    }

    static MenuHover FindMenuHover(HWND window)
    {
        MenuHover hover;
        if (g_controlsPage != 0)
            return hover;
        std::vector<MenuRow> rows;
        MenuGrid grid;
        {
            std::lock_guard lock(g_rowsMutex);
            auto now = std::chrono::steady_clock::now();
            if (now - g_gridTime <= std::chrono::milliseconds(300))
                grid = g_grid;
            if (now - g_rowsTime > std::chrono::milliseconds(300) && grid.columns == 0)
                return hover;
            rows = g_rows;
        }
        float x, y;
        if (!CursorOnPicture(window, x, y))
            return hover;

        if (g_optionsVisible)
        {
            hover.hasRows = hover.options = true;
            bool gameOpen = OptionsGameOpen();
            int line = g_editField;
            if (gameOpen || line >= 0)
            {
                // The lines of the row opened.
                float top = gameOpen ? OptionsGameRowY(OptionsGameRow()) : OptionsExtraRowY(g_extraRow);
                if (gameOpen)
                    top += OptionsShift(top); // Lower than the title thinks, below the rows added.
                hover.selected = gameOpen ? OptionsGameLine() : line;
                if (x >= OPTIONS_X && x < OPTIONS_X + OPTIONS_WIDTH * OPEN_WIDTH_SCALE)
                {
                    int index = int(std::floor((y - top + OPTIONS_STEP * 0.5f) / OPTIONS_STEP));
                    if (index >= 0 && index <= (gameOpen ? g_gameOpenLines.load() : 2))
                        hover.target = index;
                    if (hover.target == hover.selected && x >= 505.0f && x < 548.0f)
                        hover.arrow = x < 527.0f ? -1 : 1;
                }
                return hover;
            }
            int extra = g_extraRow;
            hover.selected = extra >= 0 ? ExtraRowDisplay(extra) : GameRowDisplay(OptionsGameRow());
            if (x >= OPTIONS_X && x < OPTIONS_X + OPTIONS_WIDTH)
            {
                for (int i = 0; i < DisplayRows(); i++)
                {
                    if (std::abs(y - DisplayRowY(i)) < OPTIONS_STEP * 0.5f)
                        hover.target = i;
                }
            }
            return hover;
        }

        float column = 0.0f;
        bool found = false;
        // The frontmost list: a window that pops up is drawn after what's behind it.
        for (auto row = rows.rbegin(); row != rows.rend(); ++row)
        {
            if (row->selected)
            {
                column = row->x0;
                found = true;
                break;
            }
        }
        if (!found)
        {
            // Not a list: a grid, if there's one.
            if (grid.columns > 0 && grid.selected >= 0)
            {
                hover.hasRows = true;
                hover.columns = grid.columns;
                hover.selected = grid.selected;
                int gridColumn = int(std::floor((x - grid.x) / grid.stepX)), gridRow = int(std::floor((y - grid.y) / grid.stepY));
                if (gridColumn >= 0 && gridColumn < grid.columns && gridRow >= 0 && gridRow < grid.rows)
                    hover.target = gridRow * grid.columns + gridColumn;
                if (std::abs(y - grid.arrowY) < 20.0f)
                {
                    if (grid.arrowLeftX >= 0.0f && std::abs(x - grid.arrowLeftX) < 16.0f)
                        hover.pageButton = XINPUT_GAMEPAD_LEFT_SHOULDER;
                    if (grid.arrowRightX >= 0.0f && std::abs(x - grid.arrowRightX) < 16.0f)
                        hover.pageButton = XINPUT_GAMEPAD_RIGHT_SHOULDER;
                }
            }
            return hover;
        }
        size_t allRows = rows.size();
        std::erase_if(rows, [&](const MenuRow& row) { return std::abs(row.x0 - column) > 1.0f; });
        if (rows.size() == 2 && rows.size() < allRows && std::abs(column - 777.0f) < 3.0f)
            hover.closeRow = 1;
        else if (rows.size() == 1 && rows[0].window)
            hover.closeRow = 0;
        std::sort(rows.begin(), rows.end(), [](const MenuRow& a, const MenuRow& b) { return a.y0 < b.y0; });
        hover.hasRows = true;

        for (size_t i = 0; i < rows.size(); i++)
        {
            if (rows[i].selected)
                hover.selected = int(i);
            if (x >= rows[i].x0 && x < rows[i].x1 && y >= rows[i].y0 && y < rows[i].y1)
                hover.target = int(i);
        }
        return hover;
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

    // In a window the cursor is only kept in during play, where it's hidden and turns the camera. In menus it's free to go to the title bar and move the window.
    static bool ClipWanted()
    {
        bool windowed = gpu::g_videoSettings.mode == gpu::DisplayMode::Windowed;
        return g_captured && !g_moving && (!windowed || !InMenu() || ViewerActive());
    }

    static void UpdateClip(HWND window)
    {
        bool wanted = ClipWanted();
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
                input.header.dwType == RIM_TYPEMOUSE)
            {
                if (!(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                {
                    g_mouseX += input.data.mouse.lLastX;
                    g_mouseY += input.data.mouse.lLastY;
                }
                if (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                    g_wheel += SHORT(input.data.mouse.usButtonData);
            }
            return false; // DefWindowProc cleans up after WM_INPUT.
        }

        case WM_ACTIVATE:
        {
            bool borderless = gpu::g_videoSettings.mode != gpu::DisplayMode::Windowed;
            if (LOWORD(wParam) == WA_INACTIVE)
                SetCaptured(window, false);
            else if (borderless)
                SetCaptured(window, true);
            return false;
        }

        case WM_LBUTTONDOWN:
            if (!g_captured)
            {
                // In menus the click counts: the cursor shows, so it's clear what it's for.
                g_ignoreLeftButton = !InMenu();
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
            // Hidden in gameplay. In menus the system's cursor stays (it can't be drawn any more directly than that).
            if (g_captured && LOWORD(lParam) == HTCLIENT && (!InMenu() || ViewerActive()))
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

    // The keyboard page: choosing a row, and then waiting up to five seconds for a key for it.
    static void HandleKeyboardPage(uint16_t pressed, bool& hoverActive)
    {
        auto isDown = [](int key) { return KeyIsDown(key); };
        // Alt gives the mouse back, the Windows keys belong to Windows, and the keys for "either of the two" have their own left and right.
        auto canBind = [](int key)
            {
                return key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU && key != VK_LMENU && key != VK_RMENU && key != VK_LWIN && key != VK_RWIN && key != VK_CANCEL;
            };
        static bool downBefore[256];
        static int64_t waitUntil;
        static int waitForRelease;
        int64_t now = Milliseconds();

        if (g_pageWaitingForKey)
        {
            int found = 0;
            for (int key = 1; key < 255; key++)
            {
                if (!canBind(key) || (IsMouseKey(key) && !g_captured))
                    continue;
                bool isNowDown = isDown(key);
                if (isNowDown && !downBefore[key] && found == 0)
                    found = key;
                downBefore[key] = isNowDown;
            }
            if (found != 0)
            {
                BindAction(PAGE_LAYOUT[g_pageColumn][g_pageRow], found);
                SaveBindings();
                g_pageWaitingForKey = false;
                waitForRelease = found;
                PlayOptionsSound(545);
            }
            else if (now >= waitUntil)
            {
                g_pageWaitingForKey = false;
                PlayOptionsSound(543);
            }
            return;
        }
        // The key just bound doesn't also do what it does here.
        if (waitForRelease != 0)
        {
            if (isDown(waitForRelease))
                return;
            waitForRelease = 0;
        }

        // T: everything back to the defaults.
        static bool resetBefore;
        bool reset = isDown('T');
        if (reset && !resetBefore)
        {
            SetDefaultBindings();
            SaveBindings();
            PlayOptionsSound(545);
        }
        resetBefore = reset;
        if (reset)
            return;

        // The row under the cursor.
        int column = g_pageColumn, row = g_pageRow;
        int hoverColumn = -1, hoverRow = -1;
        float x, y;
        if (CursorOnPicture(g_window, x, y))
        {
            for (int c = 0; c < PAGE_COLUMNS; c++)
            {
                int r = int(std::floor((y - PAGE_TOP) / PAGE_STEP));
                if (x >= PAGE_COLUMN_X[c] && x < PAGE_COLUMN_X[c] + PAGE_COLUMN_WIDTH && r >= 0 && r < PAGE_ROWS && PAGE_LAYOUT[c][r] >= 0)
                {
                    hoverColumn = c;
                    hoverRow = r;
                }
            }
        }
        bool clicked = (pressed & XINPUT_GAMEPAD_A) && isDown(VK_LBUTTON);
        if ((hoverActive || clicked) && hoverColumn >= 0)
        {
            column = hoverColumn;
            row = hoverRow;
        }

        // Directions: over the rows that can be chosen.
        int moveRow = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 0;
        int moveColumn = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
        if (moveRow != 0 || moveColumn != 0)
        {
            hoverActive = false;
            if (moveColumn != 0)
                column = (column + moveColumn + PAGE_COLUMNS) % PAGE_COLUMNS;
            if (moveRow != 0)
                row = (row + moveRow + PAGE_ROWS) % PAGE_ROWS;
            if (PAGE_LAYOUT[column][row] < 0)
                row = moveRow > 0 ? 0 : PAGE_ROWS - 2;
        }
        if (column != g_pageColumn || row != g_pageRow)
        {
            g_pageColumn = column;
            g_pageRow = row;
            PlayOptionsSound(544);
        }

        if ((pressed & XINPUT_GAMEPAD_A) && (!clicked || hoverColumn >= 0))
        {
            // Keys held now, the click that chose this row among them, count only once they've gone up and down again.
            for (int key = 1; key < 255; key++)
                downBefore[key] = isDown(key);
            waitUntil = now + 5000;
            g_pageWaitingForKey = true;
            PlayOptionsSound(542);
        }
        else if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START))
        {
            g_controlsPage = 0;
            PlayOptionsSound(543);
        }
    }

    // Mouse speed in counts per second, turned into a stick position, past the title's stick dead zone.
    static float MouseToStick(float countsPerSecond, int speedSetting)
    {
        static const float fullSpeed = AR_ENV("AR_MOUSE_FULL_SPEED") ? float(atof(AR_ENV("AR_MOUSE_FULL_SPEED"))) : 3000.0f;
        static const float deadZone = AR_ENV("AR_MOUSE_DEAD_ZONE") ? float(atof(AR_ENV("AR_MOUSE_DEAD_ZONE"))) : 0.41f;
        // (The setting's own effect, 0.6 to 1.5 times, comes on top, in the title.)
        float amount = countsPerSecond / fullSpeed * g_sensitivity * std::sqrt(float(speedSetting) / 5.0f);
        if (amount == 0.0f)
            return 0.0f;
        float magnitude = std::min(deadZone + std::abs(amount) * (1.0f - deadZone), 1.0f);
        return amount < 0.0f ? -magnitude : magnitude;
    }

    // The camera object's own copies of its settings, which turning the camera reads: invert vertical (+5504) and horizontal (+5508),
    // speed of the normal (+6100) and aiming camera (+6104). The title sets them again from what it saves at the start of each update
    // (sub_82E693B8, by the player's flags at +14720 of what +6208 points to), the speeds last, which it gets from sub_82E5EFC8, and
    // goes on to turn the camera. While the mouse turns the camera they are the mouse's from there on, which changes nothing it saves.
    static constexpr uint32_t CAMERA_OBJECT = 0x831C51C0, CAMERA_INVERT_Y = CAMERA_OBJECT + 5504, CAMERA_INVERT_X = CAMERA_OBJECT + 5508,
        CAMERA_PLAYER = CAMERA_OBJECT + 6208, CAMERA_SPEED_TABLE = 0x820ADEF8;
    static std::atomic<int64_t> g_mouseTurnedTime;

    static uint32_t CameraPlayerFlags()
    {
        uint32_t player = GuestWord(CAMERA_PLAYER);
        return player >= 0x40000000 ? GuestWord(player + 14720) : 0;
    }

    bool MouseCameraSpeed(bool aiming, float& speed)
    {
        if (!Enabled() || Milliseconds() - g_mouseTurnedTime.load() > 300)
            return false;
        auto store = [](uint32_t address, uint32_t value) { *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(address)) = value; };
        const MouseCamera& camera = g_mouseCamera[(CameraPlayerFlags() & 0x02000000) != 0 ? 1 : 0];
        store(CAMERA_INVERT_Y, camera.invertY ? 1 : 0);
        store(CAMERA_INVERT_X, camera.invertX ? 1 : 0);
        uint32_t bits = GuestWord(CAMERA_SPEED_TABLE + uint32_t(g_mouseCamera[aiming ? 1 : 0].speed - 1) * 4);
        memcpy(&speed, &bits, 4);
        return true;
    }

    bool TakeViewerMouse(float& turn, float& tilt)
    {
        int32_t dx = g_viewerMouseX.exchange(0), dy = g_viewerMouseY.exchange(0);
        if (dx == 0 && dy == 0)
            return false;
        // Radians per count of the mouse, with the settings of the mouse's camera.
        static const float speed = AR_ENV("AR_VIEWER_MOUSE_SPEED") ? float(atof(AR_ENV("AR_VIEWER_MOUSE_SPEED"))) : 0.00175f;
        const MouseCamera& camera = g_mouseCamera[0];
        float amount = speed * g_sensitivity * std::sqrt(float(camera.speed.load()) / 5.0f);
        turn = float(camera.invertX ? -dx : dx) * amount;
        tilt = float(camera.invertY != g_invertY ? dy : -dy) * amount; // As the stick: pushed down, the mouse moved down.
        return true;
    }

    // Left and right click: X and Y, or together X and A, the 360 attack. The first of them waits a moment for the other, so that
    // clicking both at once doesn't start a normal or heavy attack first.
    static uint16_t CombineAttackClicks(bool left, bool right, std::chrono::steady_clock::time_point now)
    {
        static const auto waitTime = std::chrono::milliseconds(AR_ENV("AR_CLICK_CHORD_MS") ? atoi(AR_ENV("AR_CLICK_CHORD_MS")) : 35);
        static bool wasLeft, wasRight, together;
        static uint16_t waiting, active;
        static auto waitUntil = std::chrono::steady_clock::time_point{}, tapUntil = std::chrono::steady_clock::time_point{};
        bool leftWentDown = left && !wasLeft, rightWentDown = right && !wasRight;
        wasLeft = left;
        wasRight = right;
        if (left && right)
        {
            together = true;
            waiting = active = 0;
            return XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_A;
        }
        if (together)
        {
            // The one still held doesn't attack on its own.
            together = left || right;
            return 0;
        }
        if (leftWentDown || rightWentDown)
        {
            waiting = leftWentDown ? XINPUT_GAMEPAD_X : XINPUT_GAMEPAD_Y;
            waitUntil = now + waitTime;
        }
        if (waiting != 0 && now >= waitUntil)
        {
            active = waiting;
            waiting = 0;
            tapUntil = now + std::chrono::milliseconds(50); // A quick click still is a press the title sees.
        }
        if (active != 0 && !(active == XINPUT_GAMEPAD_X ? left : right) && now >= tapUntil)
            active = 0;
        return active;
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
        else if (kind == "cursor")
        {
            float x = 0.0f, y = 0.0f;
            words >> x >> y;
            g_testCursorX = x;
            g_testCursorY = y;
        }
        else if (kind == "mouse")
        {
            int dx = 0, dy = 0;
            words >> dx >> dy;
            g_mouseX += dx;
            g_mouseY += dy;
        }
    }

    // Skipping cutscenes.
    static std::atomic<int> g_moviesOpen;
    static std::atomic<bool> g_movieSkip;
    static std::atomic<bool> g_moviePromptKeyboard;
    static std::atomic<int64_t> g_moviePromptUntil, g_movieSkipTime, g_movieBlackSince, g_movieClosedTime;
    static constexpr int64_t MOVIE_FADE_MS = 250, MOVIE_PROMPT_MS = 4000, MOVIE_CHAIN_MS = 2500, MOVIE_BLACK_LIMIT_MS = 10000;

    void NoteMovieOpen(bool open)
    {
        static const bool enabled = !AR_ENV("AR_NO_MOVIE_SKIP");
        if (!enabled)
            return;
        int64_t now = Milliseconds();
        if (open)
        {
            // A cutscene can be several movies in a row: skipping goes on through them.
            if (g_moviesOpen.fetch_add(1) == 0 && now - g_movieClosedTime.load() > MOVIE_CHAIN_MS)
                g_movieSkip = false;
            if (g_movieSkip)
                g_movieBlackSince = now;
            LOGF_INFO("movie opened, skipping {}, {} ms after skipping", g_movieSkip.load(), now - g_movieSkipTime.load());
        }
        else if (g_moviesOpen.fetch_sub(1) == 1)
        {
            g_movieClosedTime = now;
            g_moviePromptUntil = 0;
            LOGF_INFO("movie closed, {} ms after skipping", now - g_movieSkipTime.load());
        }
    }

    bool MovieSkipping()
    {
        return g_movieSkip && Milliseconds() - g_movieBlackSince.load() < MOVIE_BLACK_LIMIT_MS;
    }

    bool MovieSkippingOpen()
    {
        return MovieSkipping() && g_moviesOpen > 0;
    }

    static void AddMovieOverlay(std::vector<gpu::d3d12::OverlayQuad>& quads)
    {
        int64_t now = Milliseconds();
        if (MovieSkipping())
        {
            // Black until the title has moved on, a moment after the last movie closes.
            bool done = g_moviesOpen == 0 && now - g_movieClosedTime.load() > MOVIE_CHAIN_MS;
            if (done)
                g_movieSkip = false;
            else
            {
                float alpha = std::min(float(now - g_movieSkipTime.load()) / float(MOVIE_FADE_MS), 1.0f);
                quads.push_back({ 0.0f, 0.0f, 1280.0f, 720.0f, -1, { 0.0f, 0.0f, 0.0f, alpha } });
                return;
            }
        }
        if (g_moviesOpen > 0 && now < g_moviePromptUntil.load())
        {
            float alpha = std::min(float(g_moviePromptUntil.load() - now) / 400.0f, 1.0f);
            const float color[4] = { 1.0f, 1.0f, 1.0f, alpha }, shadow[4] = { 0.0f, 0.0f, 0.0f, alpha };
            const char* text = g_moviePromptKeyboard ? "Press ESC to skip" : "Press START to skip";
            AddPageText(quads, 1241.0f, 667.0f, text, 1, shadow, true);
            AddPageText(quads, 1240.0f, 666.0f, text, 1, color, true);
        }
    }

    // Before the rest of the input: what's pressed during a movie. Returns the buttons not to pass on.
    static uint16_t HandleMovieInput(const XINPUT_GAMEPAD& pad)
    {
        bool front = g_window != nullptr && (g_testInput || GetForegroundWindow() == g_window);
        bool keyboard = Enabled() && front;
        bool escape = keyboard && KeyIsDown(VK_ESCAPE);
        bool start = (pad.wButtons & XINPUT_GAMEPAD_START) != 0;
        static bool escapeBefore, startBefore, holding;
        bool escapePressed = escape && !escapeBefore, startPressed = start && !startBefore;
        escapeBefore = escape;
        startBefore = start;
        if (g_moviesOpen > 0)
        {
            bool padUsed = pad.wButtons != 0 || pad.bLeftTrigger > 30 || pad.bRightTrigger > 30;
            bool keyUsed = false;
            if (keyboard)
            {
                for (int i = 0; i < ACTION_COUNT; i++)
                {
                    for (int j = 0; j < 2; j++)
                    {
                        int key = g_keys[i][j];
                        keyUsed |= key != 0 && KeyIsDown(key) && (!IsMouseKey(key) || g_captured);
                    }
                }
            }
            if (padUsed || keyUsed)
            {
                g_moviePromptKeyboard = keyUsed && !padUsed;
                g_moviePromptUntil = Milliseconds() + MOVIE_PROMPT_MS;
            }
            if ((escapePressed || startPressed) && !g_movieSkip)
            {
                g_movieSkipTime = Milliseconds();
                g_movieBlackSince = g_movieSkipTime.load();
                g_movieSkip = true;
                LOGF_INFO("skipping the movie");
            }
            if (escapePressed || startPressed)
                holding = true;
        }
        // Not also the title's Pause or Back, for as long as it's held.
        holding = holding && (escape || start);
        return holding ? uint16_t(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_B) : uint16_t(0);
    }

    static void ApplyKeysAndMouse(XINPUT_STATE& state, bool padConnected);

    void Apply(XINPUT_STATE& state, bool padConnected)
    {
        uint16_t blocked = HandleMovieInput(state.Gamepad);
        ApplyKeysAndMouse(state, padConnected);
        state.Gamepad.wButtons &= ~blocked;
    }

    static void ApplyKeysAndMouse(XINPUT_STATE& state, bool padConnected)
    {
        auto& pad = state.Gamepad;
        // Without a controller the keyboard and mouse's icons show from the start.
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
            g_wheel = 0;
            return;
        }
        if (!g_testInput)
            UpdateClip(g_window);

        // Reading the settings once.
        static bool settingsLoaded;
        if (!settingsLoaded)
        {
            settingsLoaded = true;
            LoadMouseSettings();
            LoadBindings();
        }

        bool menu = InMenu();
        auto down = [](int key) { return KeyIsDown(key); };
        bool altDown = down(VK_MENU);
        if (g_ignoreLeftButton && !down(VK_LBUTTON))
            g_ignoreLeftButton = false;

        bool viewer = ViewerActive();
        MenuHover hover = menu && !viewer ? FindMenuHover(g_window) : MenuHover{};
        auto keyTime = std::chrono::steady_clock::now();
        auto actionDown = [&](int action)
            {
                int key = g_keys[action][0];
                return key != 0 && down(key) && (!IsMouseKey(key) || g_captured);
            };

        // What each key does now, in gameplay and in menus.
        uint16_t keyGameplay[256] = {}, keyMenu[256] = {};
        for (int i = 0; i < ACTION_COUNT; i++)
        {
            int first = g_keys[i][0], second = g_keys[i][1];
            keyGameplay[first] |= ACTIONS[i].gameplay;
            keyMenu[first] |= ACTIONS[i].menu;
            keyGameplay[second] |= ACTIONS[i].secondGameplay;
            keyMenu[second] |= ACTIONS[i].secondGameplay == TRIGGER_RIGHT ? TRIGGER_RIGHT : ACTIONS[i].menu;
        }
        keyGameplay[0] = keyMenu[0] = 0;
        // The key the weapon shares with Heavy Attack is only its usual self in menus (right click goes back).
        int weaponAttackKey = WeaponHeldOnAttackKey() ? g_keys[ACT_KILLER][0].load() : 0;
        if (weaponAttackKey != 0)
            keyMenu[weaponAttackKey] &= ~TRIGGER_LEFT;
        for (const MenuKey& key : MENU_KEYS)
            keyMenu[key.key] |= key.buttons;
        // The character viewer: W and S zoom (as the left stick, below), T and G move the camera up and down, whatever else they're bound to.
        if (viewer)
        {
            for (int key : { 'W', 'A', 'S', 'D', 'T', 'G', 'V' })
                keyMenu[key] = 0;
            keyMenu['T'] = XINPUT_GAMEPAD_DPAD_UP;
            keyMenu['G'] = XINPUT_GAMEPAD_DPAD_DOWN;
        }

        // What each held key was bound to when it went down.
        static uint16_t held[256];
        static bool wasDown[256];
        static bool clickWaiting; // A click on a row the highlight hasn't reached yet.
        static auto clickUntil = std::chrono::steady_clock::time_point{};
        uint16_t buttons = 0;
        bool attackLeft = false, attackRight = false, weaponAttackDown = false;
        int normalKey = g_keys[ACT_NORMAL][0], heavyKey = g_keys[ACT_HEAVY][0];
        for (int key = 1; key < 256; key++)
        {
            if (keyGameplay[key] == 0 && keyMenu[key] == 0 && !wasDown[key])
                continue;
            bool pressed = !altDown && down(key) && (!IsMouseKey(key) || g_captured) && !(key == VK_LBUTTON && g_ignoreLeftButton);
            if (pressed && !wasDown[key])
            {
                held[key] = menu ? keyMenu[key] : keyGameplay[key];
                // Left click on a list: on a row, or nothing happens. On another row than the selected one, once it's selected.
                if (key == VK_LBUTTON && menu && hover.hasRows)
                {
                    if (hover.pageButton != 0)
                        held[key] = hover.pageButton;
                    else if (hover.target < 0)
                        held[key] = 0;
                    else if (hover.target != hover.selected)
                    {
                        held[key] = 0;
                        clickWaiting = true;
                    }
                    else if (hover.arrow != 0)
                        held[key] = hover.arrow > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
                }
            }
            wasDown[key] = pressed;
            if (pressed)
            {
                if (key == weaponAttackKey && held[key] == (TRIGGER_LEFT | XINPUT_GAMEPAD_Y))
                    weaponAttackDown = true;
                else if (key == normalKey && held[key] == XINPUT_GAMEPAD_X)
                    attackLeft = true;
                else if (key == heavyKey && held[key] == XINPUT_GAMEPAD_Y)
                    attackRight = true;
                else
                    buttons |= held[key];
            }
        }
        // The key both the weapon and Heavy Attack use. Held, it readies the weapon, at once if the attack key goes down meanwhile (that's firing).
        // Tapped, it's the attack as it comes back up. With the attack key at the same moment, it's the 360 attack as usual.
        {
            static const auto holdTime = std::chrono::milliseconds(AR_ENV("AR_TAP_HOLD_MS") ? atoi(AR_ENV("AR_TAP_HOLD_MS")) : 150);
            static const auto chordTime = std::chrono::milliseconds(AR_ENV("AR_CLICK_CHORD_MS") ? atoi(AR_ENV("AR_CLICK_CHORD_MS")) : 35);
            static bool wasDown, wasLeft, weapon, chord;
            static auto downTime = std::chrono::steady_clock::time_point{}, leftTime = downTime, tapUntil = downTime;
            if (attackLeft && !wasLeft)
                leftTime = keyTime;
            wasLeft = attackLeft;
            if (weaponAttackDown && !wasDown)
            {
                downTime = keyTime;
                weapon = chord = false;
            }
            if (weaponAttackDown && !weapon && !chord)
            {
                if (attackLeft)
                {
                    auto apart = downTime > leftTime ? downTime - leftTime : leftTime - downTime;
                    (g_keys[ACT_360][0] == 0 && apart <= chordTime ? chord : weapon) = true;
                }
                else if (keyTime - downTime >= holdTime)
                    weapon = true;
            }
            if (wasDown && !weaponAttackDown && !weapon && !chord)
                tapUntil = keyTime + std::chrono::milliseconds(60); // A press the title sees.
            wasDown = weaponAttackDown;
            if (weaponAttackDown && weapon)
            {
                buttons |= TRIGGER_LEFT;
                // Aiming, the title fires on the right trigger: left click, while right click holds the aim. (Away from aiming, it's a weapon's attacks: X.)
                static const bool fireFix = !AR_ENV("AR_NO_CLICK_FIRE");
                if (fireFix && attackLeft && !menu && (CameraPlayerFlags() & 0x0A000000) != 0)
                {
                    buttons |= TRIGGER_RIGHT;
                    attackLeft = false;
                }
            }
            if (keyTime < tapUntil)
                buttons |= XINPUT_GAMEPAD_Y;
            attackRight |= weaponAttackDown && chord;
        }
        // Together they're the 360 attack, unless it has a key of its own.
        if (g_keys[ACT_360][0] == 0)
            buttons |= CombineAttackClicks(attackLeft, attackRight, keyTime);
        else
            buttons |= (attackLeft ? XINPUT_GAMEPAD_X : 0) | (attackRight ? XINPUT_GAMEPAD_Y : 0);

        // Moving the mouse takes over the selection, until a key moves it again.
        static bool hoverActive;
        static POINT lastCursor;
        POINT cursor{};
        if (g_testInput)
            cursor = { LONG(g_testCursorX * 16.0f), LONG(g_testCursorY * 16.0f) };
        else
            GetCursorPos(&cursor);
        if (cursor.x != lastCursor.x || cursor.y != lastCursor.y)
        {
            lastCursor = cursor;
            hoverActive = true;
        }
        if (!menu)
            hoverActive = clickWaiting = false;
        // Arriving at the Options screen, or closing a row, the mouse takes over only once it moves again.
        static bool optionsWere;
        if (g_optionsVisible != optionsWere)
        {
            optionsWere = g_optionsVisible;
            hoverActive = false;
        }
        if ((buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) != 0 || actionDown(ACT_MOVE_FORWARD) || actionDown(ACT_MOVE_BACK) || std::abs(pad.sThumbLY) > 8000)
            hoverActive = clickWaiting = false;
        if (hover.columns > 0 && ((buttons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) != 0 || actionDown(ACT_MOVE_LEFT) || actionDown(ACT_MOVE_RIGHT) || std::abs(pad.sThumbLX) > 8000))
            hoverActive = clickWaiting = false;

        // AR_LOG_BUTTONS: where the mouse is in a list or grid, as it changes.
        {
            static const bool logHover = AR_ENV("AR_LOG_BUTTONS") != nullptr;
            static int hoverBefore[4];
            int hoverNow[4] = { hover.selected, hover.target, hover.columns, hoverActive ? 1 : 0 };
            if (logHover && memcmp(hoverNow, hoverBefore, sizeof(hoverNow)) != 0)
            {
                memcpy(hoverBefore, hoverNow, sizeof(hoverNow));
                LOGF_INFO("hover selected {} target {} columns {} active {}", hoverNow[0], hoverNow[1], hoverNow[2], hoverNow[3]);
            }
        }

        static bool closingWindow, backBefore;
        bool back = (buttons & XINPUT_GAMEPAD_B) != 0;
        if (hover.closeRow >= 0)
        {
            if (back && !backBefore)
            {
                closingWindow = true;
                hoverActive = false;
            }
            buttons &= ~XINPUT_GAMEPAD_B;
        }
        else
            closingWindow = false;
        backBefore = back;
        static int pressedFrom = -1;
        static auto pressedUntil = std::chrono::steady_clock::time_point{};
        static uint16_t hoverButton;
        static auto hoverUntil = std::chrono::steady_clock::time_point{};
        // A row that can't be selected (locked levels are skipped by the title): the selection jumps past it, and that's the end of trying.
        static int unreachable = -1, tryingFrom = -1, tryingFor = -1, tryingWay = 0;
        if (hover.target != unreachable)
            unreachable = -1;
        if (tryingWay != 0 && hover.selected >= 0 && hover.selected != tryingFrom)
        {
            bool passed = tryingWay > 0 ? (hover.selected > tryingFor || hover.selected < tryingFrom) : (hover.selected < tryingFor || hover.selected > tryingFrom);
            if (passed && hover.columns == 0 && hover.target == tryingFor)
                unreachable = tryingFor;
            tryingWay = 0;
        }
        if (unreachable >= 0)
            clickWaiting = false;
        if (keyTime >= hoverUntil)
        {
            if (hoverButton != 0)
            {
                hoverButton = 0;
                hoverUntil = keyTime + std::chrono::milliseconds(40);
            }
            else if (closingWindow)
            {
                if (hover.selected == hover.closeRow)
                {
                    closingWindow = false;
                    clickUntil = keyTime + std::chrono::milliseconds(60);
                }
                else if (hover.selected != pressedFrom || keyTime >= pressedUntil)
                {
                    pressedFrom = hover.selected;
                    pressedUntil = keyTime + std::chrono::milliseconds(350);
                    hoverButton = XINPUT_GAMEPAD_DPAD_DOWN;
                    hoverUntil = keyTime + std::chrono::milliseconds(50);
                }
            }
            else if ((hoverActive || clickWaiting) && hover.target >= 0 && hover.selected >= 0 && hover.target != hover.selected && hover.target != unreachable &&
                (!hover.options || OptionsGameEditing()) && (hover.selected != pressedFrom || keyTime >= pressedUntil))
            {
                tryingFrom = hover.selected;
                tryingFor = hover.target;
                tryingWay = hover.target > hover.selected ? 1 : -1;
                // One press, then wait for the selection to follow before the next.
                pressedFrom = hover.selected;
                pressedUntil = keyTime + std::chrono::milliseconds(350);
                hoverButton = hover.target > hover.selected ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_UP;
                if (hover.columns > 0)
                {
                    // Along the row first, then along the column.
                    int fromColumn = hover.selected % hover.columns, toColumn = hover.target % hover.columns;
                    if (fromColumn != toColumn)
                        hoverButton = toColumn > fromColumn ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
                }
                hoverUntil = keyTime + std::chrono::milliseconds(50);
            }
            else if (clickWaiting && hover.target >= 0 && hover.target == hover.selected)
            {
                clickWaiting = false;
                clickUntil = keyTime + std::chrono::milliseconds(60);
            }
        }
        buttons |= hoverButton;
        if (keyTime < clickUntil)
            buttons |= XINPUT_GAMEPAD_A;

        // The wheel: weapon slots in gameplay (D-pad left and right), the cursor in menus. A notch is a press long enough for the
        // title to see, and a gap before the next.
        static int wheelNotches;
        static uint16_t wheelButton;
        static auto wheelUntil = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        int32_t wheel = g_wheel.load();
        if (std::abs(wheel) >= WHEEL_DELTA)
        {
            int notches = wheel / WHEEL_DELTA;
            g_wheel -= notches * WHEEL_DELTA;
            wheelNotches = std::clamp(wheelNotches + notches, -3, 3);
        }
        if (now >= wheelUntil)
        {
            if (wheelButton != 0)
            {
                wheelButton = 0;
                wheelUntil = now + std::chrono::milliseconds(50);
            }
            else if (wheelNotches != 0)
            {
                bool up = wheelNotches > 0;
                wheelNotches += up ? -1 : 1;
                bool rowOpen = g_optionsVisible && (OptionsGameEditing() || g_editField >= 0);
                wheelButton = viewer ? 0 : rowOpen ? (up ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT)
                    : menu ? (up ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN) : (up ? XINPUT_GAMEPAD_DPAD_LEFT : XINPUT_GAMEPAD_DPAD_RIGHT);
                wheelUntil = now + std::chrono::milliseconds(70);
            }
        }
        buttons |= wheelButton;

        // Left stick.
        int moveX = 0, moveY = 0;
        if (!altDown)
        {
            moveX = int(actionDown(ACT_MOVE_RIGHT)) - int(actionDown(ACT_MOVE_LEFT));
            moveY = int(actionDown(ACT_MOVE_FORWARD)) - int(actionDown(ACT_MOVE_BACK));
            if (viewer)
            {
                moveX = 0;
                moveY = int(down('W')) - int(down('S'));
            }
        }
        if (moveX != 0 || moveY != 0)
        {
            float scale = moveX != 0 && moveY != 0 ? 0.7071f : 1.0f;
            pad.sThumbLX = SHORT(float(moveX) * scale * 32767.0f);
            pad.sThumbLY = SHORT(float(moveY) * scale * 32767.0f);
        }

        // Mouse movement over the time since it was last looked at. The title may ask more than once per update, or
        // the mouse may report less often than it asks: short intervals keep the previous speed.
        static auto lastSample = std::chrono::steady_clock::now();
        static float stickX, stickY;
        float elapsed = std::chrono::duration<float>(now - lastSample).count();
        bool mouseMoved = false;
        if (elapsed >= 0.004f)
        {
            lastSample = now;
            int32_t dx = g_mouseX.exchange(0), dy = g_mouseY.exchange(0);
            mouseMoved = std::abs(dx) + std::abs(dy) > 1;
            // The character viewer takes it as it is.
            if (viewer)
            {
                g_viewerMouseX += dx;
                g_viewerMouseY += dy;
                dx = dy = 0;
            }
            // Which camera's speed, as sub_82E5F080 picks it.
            int speedSetting = g_mouseCamera[!menu && (CameraPlayerFlags() & 0x0A000000) != 0 ? 1 : 0].speed;
            stickX = MouseToStick(float(dx) / elapsed, speedSetting);
            stickY = MouseToStick(float(g_invertY ? dy : -dy) / elapsed, speedSetting);
        }
        // In menus the mouse doesn't work the camera. Turning it uses the mouse's settings: the sign of the stick stays as it is,
        // inverting is the title's to do, like the speed (see MouseCameraSpeed).
        if (!menu && (stickX != 0.0f || stickY != 0.0f))
        {
            pad.sThumbRX = SHORT(stickX * 32767.0f);
            pad.sThumbRY = SHORT(stickY * 32767.0f);
            g_mouseTurnedTime = Milliseconds();
        }

        if ((buttons & TRIGGER_LEFT) != 0)
            pad.bLeftTrigger = 255;
        if ((buttons & TRIGGER_RIGHT) != 0)
            pad.bRightTrigger = 255;
        pad.wButtons |= buttons & ~(TRIGGER_LEFT | TRIGGER_RIGHT);

        if (buttons != 0 || moveX != 0 || moveY != 0 || mouseMoved)
            g_active = true;

        // The rows added to Options and the controls pages take the input from here on. Sticks count as directions, repeating.
        {
            static uint16_t lastHeld;
            static auto repeatAt = std::chrono::steady_clock::time_point{};
            static bool backThrough;
            uint16_t current = pad.wButtons;
            if (pad.sThumbLY > 16000)
                current |= XINPUT_GAMEPAD_DPAD_UP;
            if (pad.sThumbLY < -16000)
                current |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (pad.sThumbLX > 16000)
                current |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (pad.sThumbLX < -16000)
                current |= XINPUT_GAMEPAD_DPAD_LEFT;
            uint16_t pressed = current & ~lastHeld;
            constexpr uint16_t DIRECTIONS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
            if (pressed & DIRECTIONS)
                repeatAt = now + std::chrono::milliseconds(350);
            else if ((current & DIRECTIONS) && now >= repeatAt)
            {
                pressed |= current & DIRECTIONS;
                repeatAt = now + std::chrono::milliseconds(110);
            }
            lastHeld = current;

            // What was pressed for the added rows doesn't reach the title when it's held on to.
            static uint16_t swallowed;
            auto silence = [&]
                {
                    swallowed |= current;
                    pad.wButtons = 0;
                    pad.sThumbLX = pad.sThumbLY = pad.sThumbRX = pad.sThumbRY = 0;
                    pad.bLeftTrigger = pad.bRightTrigger = 0;
                };

            // Moving through Options, added rows included: to a row in the order shown, by pressing up or down for the title one
            // press at a time, waiting for its cursor to follow.
            static int want = -1;
            static bool pressedForWant;
            static uint16_t pulse;
            static auto pulseUntil = std::chrono::steady_clock::time_point{}, pulseRetry = std::chrono::steady_clock::time_point{};
            static int pulseFrom = -1;
            auto setWant = [&](int display)
                {
                    want = display;
                    pressedForWant = false;
                };

            if (g_controlsPage == 2)
            {
                HandleKeyboardPage(pressed, hoverActive);
                silence();
            }
            else if (g_controlsPage != 0)
            {
                if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START))
                {
                    g_controlsPage = 0;
                    PlayOptionsSound(543);
                }
                silence();
            }
            else if (g_optionsVisible)
            {
                int extra = g_extraRow;
                int row = OptionsGameRow();
                if (OptionsGameEditing())
                    want = -1; // A row of the title's opened: all the title's.
                else if (g_editField >= 0 && extra >= 0)
                {
                    // A mouse row opened.
                    int line = g_editField;
                    MouseCamera& camera = g_mouseCamera[extra];
                    int move = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 0;
                    if (move != 0)
                    {
                        line = (line + move + 3) % 3;
                        hoverActive = false;
                    }
                    else if (hoverActive && hover.options && hover.target >= 0)
                        line = hover.target;
                    if (line != g_editField)
                    {
                        g_editField = line;
                        PlayOptionsSound(544);
                    }
                    int step = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
                    if (step != 0)
                    {
                        if (line == 0)
                            camera.invertY = !camera.invertY;
                        else if (line == 1)
                            camera.invertX = !camera.invertX;
                        else
                            camera.speed = std::clamp(camera.speed + step, 1, 10);
                        PlayOptionsSound(545);
                    }
                    if (pressed & XINPUT_GAMEPAD_A)
                    {
                        SaveMouseSettings();
                        g_editField = -1;
                        hoverActive = false;
                        PlayOptionsSound(542);
                    }
                    else if (pressed & XINPUT_GAMEPAD_B)
                    {
                        camera.invertY = g_editBackupY;
                        camera.invertX = g_editBackupX;
                        camera.speed = g_editBackupSpeed;
                        g_editField = -1;
                        hoverActive = false;
                        PlayOptionsSound(543);
                    }
                    silence();
                    want = -1;
                }
                else
                {
                    int gameRows = g_gameRows;
                    int position = extra >= 0 ? ExtraRowDisplay(extra) : GameRowDisplay(row);
                    int move = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 0;
                    if (move != 0)
                    {
                        setWant(((want >= 0 ? want : position) + move + DisplayRows()) % DisplayRows());
                        hoverActive = false;
                    }
                    else if ((hoverActive || clickWaiting) && hover.options && hover.target >= 0 && hover.target != (want >= 0 ? want : position))
                        setWant(hover.target);
                    // The title only gets the presses made for it.
                    pad.wButtons &= ~(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);
                    pad.sThumbLY = 0;

                    if (extra >= 0)
                    {
                        if (pressed & XINPUT_GAMEPAD_A)
                        {
                            PlayOptionsSound(542);
                            if (extra <= ROW_MOUSE_AIMING)
                            {
                                const MouseCamera& camera = g_mouseCamera[extra];
                                g_editBackupY = camera.invertY;
                                g_editBackupX = camera.invertX;
                                g_editBackupSpeed = camera.speed;
                                g_editField = 0;
                            }
                            else
                            {
                                g_pageColumn = 0;
                                g_pageRow = 0;
                                g_pageWaitingForKey = false;
                                g_controlsPage = extra == ROW_CONTROLS_PAD ? 1 : 2;
                            }
                        }
                        if ((pressed & XINPUT_GAMEPAD_Y) && extra <= ROW_MOUSE_AIMING)
                        {
                            g_mouseCamera[extra].invertY = false;
                            g_mouseCamera[extra].invertX = false;
                            g_mouseCamera[extra].speed = 5;
                            SaveMouseSettings();
                            PlayOptionsSound(545);
                        }
                        // Back goes through, leaving Options; only a press that started here, not one that closed something.
                        if (pressed & XINPUT_GAMEPAD_B)
                            backThrough = true;
                        if (!(current & XINPUT_GAMEPAD_B))
                            backThrough = false;
                        silence();
                        if (backThrough)
                        {
                            swallowed &= ~XINPUT_GAMEPAD_B;
                            pad.wButtons = XINPUT_GAMEPAD_B;
                        }
                    }

                    if (want >= 0)
                    {
                        int wantExtra = -1, wantRow = -1;
                        for (int i = 0; i < EXTRA_ROWS; i++)
                            wantExtra = ExtraRowDisplay(i) == want ? i : wantExtra;
                        for (int i = 0; i < gameRows; i++)
                            wantRow = GameRowDisplay(i) == want ? i : wantRow;
                        int goal = row;
                        if (wantExtra >= 0)
                        {
                            bool nextToIt = wantExtra <= ROW_MOUSE_AIMING ? (row == 3 || row == 4) : row == gameRows - 1;
                            if (extra != wantExtra)
                            {
                                g_extraRow = wantExtra;
                                g_editField = -1;
                                if (nextToIt)
                                    PlayOptionsSound(544);
                            }
                            if (nextToIt)
                                want = -1;
                            else
                                goal = wantExtra <= ROW_MOUSE_AIMING ? (std::abs(row - 3) < std::abs(row - 4) ? 3 : 4) : gameRows - 1;
                        }
                        else if (row == wantRow)
                        {
                            if (extra >= 0)
                            {
                                g_extraRow = -1;
                                if (!pressedForWant)
                                    PlayOptionsSound(544); // The title's cursor was there already, and made no sound.
                            }
                            want = -1;
                        }
                        else
                            goal = wantRow;

                        if (goal != row && pulse == 0 && now >= pulseUntil && (row != pulseFrom || now >= pulseRetry))
                        {
                            // The shorter way round: the title's list wraps around.
                            int forward = (goal - row + gameRows) % gameRows;
                            pulse = forward <= gameRows - forward ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_UP;
                            pulseFrom = row;
                            pulseRetry = now + std::chrono::milliseconds(350);
                            pulseUntil = now + std::chrono::milliseconds(50);
                            pressedForWant = true;
                        }
                    }
                }
            }
            else
            {
                want = -1;
                pulseFrom = -1;
            }
            swallowed &= current;
            pad.wButtons &= ~swallowed;
            if (pulse != 0)
            {
                if (now >= pulseUntil)
                {
                    pulse = 0;
                    pulseUntil = now + std::chrono::milliseconds(40);
                }
                else
                    pad.wButtons |= pulse;
            }
        }

        // The title screen: any key, click or button does what A does there.
        {
            static bool keysBefore[256], latched, showingBefore;
            static uint16_t padBefore;
            static int64_t pressUntil;
            int64_t nowMs = Milliseconds();
            bool showing = TitlePromptShowing();
            if (showing)
            {
                bool any = (pad.wButtons & ~padBefore) != 0 || pad.bLeftTrigger > 128 || pad.bRightTrigger > 128;
                for (int key = 1; key < 255; key++)
                {
                    if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN)
                        continue;
                    bool isDown = down(key);
                    any |= isDown && !keysBefore[key] && !altDown;
                    keysBefore[key] = isDown;
                }
                // Not what was already held when the prompt came up, and only once.
                if (any && showingBefore && !latched)
                {
                    latched = true;
                    pressUntil = nowMs + 80;
                }
            }
            else
                latched = false;
            showingBefore = showing;
            padBefore = pad.wButtons;
            if (nowMs < pressUntil)
                pad.wButtons |= XINPUT_GAMEPAD_A;
        }

        // AR_LOG_BUTTONS: what the title gets, as it changes.
        static const bool logButtons = AR_ENV("AR_LOG_BUTTONS") != nullptr;
        static uint16_t lastButtons;
        static uint8_t lastTrigger;
        if (logButtons && (pad.wButtons != lastButtons || pad.bLeftTrigger != lastTrigger))
        {
            lastButtons = pad.wButtons;
            lastTrigger = pad.bLeftTrigger;
            LOGF_INFO("buttons {:04X} left trigger {} camera flags {:08X}", lastButtons, lastTrigger, CameraPlayerFlags());
        }

        // AR_LOG_OPTIONS: the state of the Options screen as it changes.
        static const bool logOptions = AR_ENV("AR_LOG_OPTIONS") != nullptr;
        if (logOptions)
        {
            int stateNow[7] = { g_optionsVisible ? 1 : 0, g_optionsLayout ? 1 : 0, OptionsGameRow(), g_gameRows.load(),
                g_gameOpenLines.load(), g_extraRow.load(), g_editField.load() };
            static int stateBefore[7];
            if (memcmp(stateNow, stateBefore, sizeof(stateNow)) != 0)
            {
                memcpy(stateBefore, stateNow, sizeof(stateNow));
                LOGF_INFO("options visible {} layout {} row {} of {} lines {} extra {} field {}", stateNow[0], stateNow[1], stateNow[2], stateNow[3], stateNow[4], stateNow[5], stateNow[6]);
            }
        }

        static const bool logDevice = AR_ENV("AR_LOG_INPUT_DEVICE") != nullptr;
        static bool lastActive;
        if (logDevice && lastActive != g_active.load())
        {
            lastActive = g_active;
            LOGF_INFO("input device: {} (buttons {:04X} move {} {} mouse {} {})", lastActive ? "keyboard and mouse" : "controller", buttons, moveX, moveY, stickX, stickY);
        }
    }
}
