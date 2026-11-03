// Controller input through XInput. Guest controller indices map directly to XInput user indices.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/xam.h>
#include <Xinput.h>
#include <sstream>
#include <gpu/d3d12_renderer.h>

// XAMINPUT structures in xbox.h use the host XInput layout; guest memory holds them big endian.
static void WriteGamepad(XAMINPUT_GAMEPAD* guest, const XINPUT_GAMEPAD& host)
{
    guest->wButtons = ByteSwap(uint16_t(host.wButtons));
    guest->bLeftTrigger = host.bLeftTrigger;
    guest->bRightTrigger = host.bRightTrigger;
    guest->sThumbLX = int16_t(ByteSwap(uint16_t(host.sThumbLX)));
    guest->sThumbLY = int16_t(ByteSwap(uint16_t(host.sThumbLY)));
    guest->sThumbRX = int16_t(ByteSwap(uint16_t(host.sThumbRX)));
    guest->sThumbRY = int16_t(ByteSwap(uint16_t(host.sThumbRY)));
}

// Watches for controllers connecting and disconnecting, which titles learn about through a notification.
static std::atomic<uint32_t> g_connectedMask{ 0xFFFFFFFF };

static uint32_t QueryConnectedMask()
{
    uint32_t mask = 0;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; user++)
    {
        XINPUT_CAPABILITIES capabilities;
        if (XInputGetCapabilities(user, XINPUT_FLAG_GAMEPAD, &capabilities) == ERROR_SUCCESS)
            mask |= 1u << user;
    }

    return mask;
}

static void StartConnectionMonitor()
{
    static std::once_flag once;
    std::call_once(once, []
        {
            g_connectedMask = QueryConnectedMask() | (AR_ENV("AR_AUTO_PRESS") || AR_ENV("AR_REMOTE") ? 1u : 0u);
            LOGF_INFO("controllers connected: mask 0x{:X}", g_connectedMask.load());

            std::thread([]
                {
                    while (true)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        uint32_t mask = QueryConnectedMask() | (AR_ENV("AR_AUTO_PRESS") || AR_ENV("AR_REMOTE") ? 1u : 0u);
                        uint32_t previous = g_connectedMask.exchange(mask);
                        if (mask != previous)
                        {
                            LOGF_INFO("controllers connected: mask 0x{:X}", mask);
                            PostNotification(XN_SYS_INPUTDEVICESCHANGED, 0);
                        }
                    }
                }).detach();
        });
}

// Keystrokes (used by menus) are synthesized from controller state, as Windows' XInputGetKeystroke doesn't work
// with every controller: presses, releases, and repeats while a button is held.
struct Keystroke
{
    uint16_t virtualKey;
    uint16_t flags;
    uint32_t user;
};

constexpr uint16_t KEYSTROKE_KEYDOWN = 0x0001;
constexpr uint16_t KEYSTROKE_KEYUP = 0x0002;
constexpr uint16_t KEYSTROKE_REPEAT = 0x0004;

struct KeystrokeState
{
    uint64_t pressed; // One bit per entry of KEYS.
    std::chrono::steady_clock::time_point repeatTime[32];
    std::deque<Keystroke> queue;
};

// VK_PAD codes and how each is read from a gamepad.
struct KeyMapping
{
    uint16_t virtualKey;
    uint16_t button;   // XINPUT_GAMEPAD_* bit, or 0 for analog inputs below.
    int analog;        // 1/2: left/right trigger; 3-6: left stick up, down, right, left; 7-10: right stick.
};

static constexpr KeyMapping KEYS[] =
{
    { 0x5800, XINPUT_GAMEPAD_A }, { 0x5801, XINPUT_GAMEPAD_B }, { 0x5802, XINPUT_GAMEPAD_X }, { 0x5803, XINPUT_GAMEPAD_Y },
    { 0x5804, XINPUT_GAMEPAD_RIGHT_SHOULDER }, { 0x5805, XINPUT_GAMEPAD_LEFT_SHOULDER }, { 0x5806, 0, 1 }, { 0x5807, 0, 2 },
    { 0x5810, XINPUT_GAMEPAD_DPAD_UP }, { 0x5811, XINPUT_GAMEPAD_DPAD_DOWN }, { 0x5812, XINPUT_GAMEPAD_DPAD_LEFT },
    { 0x5813, XINPUT_GAMEPAD_DPAD_RIGHT }, { 0x5814, XINPUT_GAMEPAD_START }, { 0x5815, XINPUT_GAMEPAD_BACK },
    { 0x5816, XINPUT_GAMEPAD_LEFT_THUMB }, { 0x5817, XINPUT_GAMEPAD_RIGHT_THUMB },
    { 0x5820, 0, 3 }, { 0x5821, 0, 4 }, { 0x5822, 0, 5 }, { 0x5823, 0, 6 },
    { 0x5830, 0, 7 }, { 0x5831, 0, 8 }, { 0x5832, 0, 9 }, { 0x5833, 0, 10 },
};

static bool IsKeyDown(const XINPUT_GAMEPAD& pad, const KeyMapping& key)
{
    constexpr int STICK = 16384;
    constexpr int TRIGGER = 64;
    switch (key.analog)
    {
    case 0: return (pad.wButtons & key.button) != 0;
    case 1: return pad.bLeftTrigger > TRIGGER;
    case 2: return pad.bRightTrigger > TRIGGER;
    case 3: return pad.sThumbLY > STICK;
    case 4: return pad.sThumbLY < -STICK;
    case 5: return pad.sThumbLX > STICK;
    case 6: return pad.sThumbLX < -STICK;
    case 7: return pad.sThumbRY > STICK;
    case 8: return pad.sThumbRY < -STICK;
    case 9: return pad.sThumbRX > STICK;
    default: return pad.sThumbRX < -STICK;
    }
}

static std::mutex g_keystrokeMutex;
static KeystrokeState g_keystrokes[XUSER_MAX_COUNT];

// Unused XINPUT_GAMEPAD bits standing for the triggers, and for holding a step's buttons for its whole period.
constexpr uint16_t AUTO_LEFT_TRIGGER = 0x0400;
constexpr uint16_t AUTO_HOLD = 0x0800;

// Debugging without a controller (AR_AUTO_PRESS=<seconds>): controller 0 taps a button every that many seconds,
// alternating A and Start.
static uint16_t AutoPressButtons(float& stickAngle)
{
    stickAngle = -1.0f;
    static const int period = AR_ENV("AR_AUTO_PRESS") ? atoi(AR_ENV("AR_AUTO_PRESS")) : 0;
    static const auto start = std::chrono::steady_clock::now();
    if (period <= 0)
        return 0;

    // AR_AUTO_FRAMES: steps go by presented frames (period * 30 per step) instead of time, so slow runs stay in step.
    static const bool byFrames = AR_ENV("AR_AUTO_FRAMES") != nullptr;
    auto elapsed = byFrames ? int64_t(gpu::d3d12::GetFrameNumber() * 1000 / 30) :
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed < period * 1000)
        return 0;
    bool tapped = elapsed % (period * 1000) < 250;

    // AR_AUTO_SEQUENCE: comma separated buttons (A, B, X, Y, START, BACK, UP, DOWN, LEFT, RIGHT, LB, RB, LT) pressed
    // in order instead (WAIT skips a turn, LTHOLD holds the left trigger for the whole turn, MOVE<degrees> holds the
    // left stick that many degrees counterclockwise from up for the whole turn); after the last one, A repeats, or
    // nothing when the list ends with WAIT.
    static std::vector<float> angles;
    static const std::vector<uint16_t> sequence = []
        {
            std::vector<uint16_t> buttons;
            static const std::pair<const char*, uint16_t> names[] = { { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B },
                { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y }, { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK },
                { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT },
                { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { "LT", AUTO_LEFT_TRIGGER }, { "LTHOLD", AUTO_LEFT_TRIGGER | AUTO_HOLD } };
            if (const char* text = AR_ENV("AR_AUTO_SEQUENCE"))
            {
                std::string list = text;
                size_t start = 0;
                while (start <= list.size())
                {
                    size_t end = list.find(',', start);
                    std::string name = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    size_t count = buttons.size();
                    if (name == "WAIT")
                        buttons.push_back(0);
                    else if (name.rfind("MOVE", 0) == 0)
                        buttons.push_back(AUTO_HOLD);
                    for (auto& [buttonName, button] : names)
                    {
                        if (name == buttonName)
                            buttons.push_back(button);
                    }
                    if (buttons.size() > count)
                        angles.push_back(name.rfind("MOVE", 0) == 0 ? float(atof(name.c_str() + 4)) : -1.0f);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
            return buttons;
        }();

    size_t step = size_t(elapsed / (period * 1000)) - 1;
    uint16_t buttons;
    if (!sequence.empty())
    {
        buttons = step < sequence.size() ? sequence[step] : sequence.back() == 0 ? 0 : XINPUT_GAMEPAD_A;
        if (step < angles.size())
            stickAngle = angles[step];
    }
    else
        buttons = (elapsed / (period * 1000)) % 2 ? XINPUT_GAMEPAD_A : XINPUT_GAMEPAD_START;

    return tapped || (buttons & AUTO_HOLD) ? buttons : 0;
}

// Remote control for debugging (AR_REMOTE=<file>): lines appended to the file while the game runs are carried out in
// order. A line lists what to hold and for how long: button names as in AR_AUTO_SEQUENCE (plus RT), L<degrees> and
// R<degrees> for the left and right stick (counterclockwise from up), and a duration in ms (150 with buttons,
// otherwise 0), e.g. "L0 A 400". A bare number waits; "shot <name>" saves the next frame to screenshots/<name>.bmp;
// "scan" starts a timer scan (AR_SCAN_TIMERS), "scanwatch" one that also watches the timers it finds; "find ..." and
// "track ..." search and log guest memory (RunFind in timing_hooks.cpp).
// Buttons are released for 60 ms after each line, so repeated lines are separate presses.
struct RemoteAction
{
    uint16_t buttons = 0;
    float left = -1.0f, right = -1.0f;
    bool leftTrigger = false, rightTrigger = false;
    int64_t milliseconds = 0;
    std::string shot; // Screenshot taken when the action starts.
    int scan = 0; // Timer scan started when the action starts (2: and watch the timers).
    std::string find; // Memory search step or tracker (see RunFind in timing_hooks.cpp).
};

void RequestTimerScan(bool watch);
void RequestFind(const std::string& request);

static bool RemoteControl(XINPUT_STATE& state)
{
    static const char* path = AR_ENV("AR_REMOTE");
    if (!path)
        return false;

    static std::mutex mutex;
    static std::deque<RemoteAction> actions;
    static size_t linesRead;
    static auto lastPoll = std::chrono::steady_clock::time_point{};
    static auto actionEnd = std::chrono::steady_clock::time_point{};
    static RemoteAction current;
    std::lock_guard lock(mutex);
    auto now = std::chrono::steady_clock::now();
    if (now - lastPoll > std::chrono::milliseconds(50))
    {
        lastPoll = now;
        std::ifstream file(path);
        std::string line;
        size_t index = 0;
        while (std::getline(file, line))
        {
            if (index++ < linesRead || line.empty())
                continue;
            linesRead = index;
            std::istringstream words(line);
            std::string word;
            RemoteAction action;
            bool timed = false;
            while (words >> word)
            {
                static const std::pair<const char*, uint16_t> names[] = { { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B },
                    { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y }, { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK },
                    { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT },
                    { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
                    { "LS", XINPUT_GAMEPAD_LEFT_THUMB }, { "RS", XINPUT_GAMEPAD_RIGHT_THUMB } };
                if (word == "shot")
                {
                    words >> action.shot;
                    if (action.shot.empty())
                        action.shot = "remote";
                    continue;
                }
                if (word == "find" || word == "track")
                {
                    std::string rest;
                    std::getline(words, rest);
                    rest.erase(0, rest.find_first_not_of(' '));
                    action.find = word == "track" ? "track " + rest : rest;
                    continue;
                }
                if (word == "scan" || word == "scanwatch")
                {
                    action.scan = word == "scan" ? 1 : 2;
                    continue;
                }
                if (word == "LT")
                    action.leftTrigger = true;
                else if (word == "RT")
                    action.rightTrigger = true;
                else if ((word[0] == 'L' || word[0] == 'R') && word.size() > 1 && (isdigit(uint8_t(word[1])) || word[1] == '-'))
                    (word[0] == 'L' ? action.left : action.right) = std::fmod(float(atof(word.c_str() + 1)) + 360.0f, 360.0f);
                else if (isdigit(uint8_t(word[0])))
                {
                    action.milliseconds = atoll(word.c_str());
                    timed = true;
                }
                for (auto& [name, button] : names)
                {
                    if (word == name)
                        action.buttons |= button;
                }
            }
            bool pressing = action.buttons || action.leftTrigger || action.rightTrigger;
            if (!timed)
                action.milliseconds = pressing ? 150 : 0;
            if (action.milliseconds > 0 || pressing || action.left >= 0.0f || action.right >= 0.0f || !action.shot.empty() || action.scan || !action.find.empty())
                actions.push_back(action);
            if (pressing)
                actions.push_back(RemoteAction{ .milliseconds = 60 });
        }
    }

    if (now >= actionEnd)
    {
        current = {};
        if (!actions.empty())
        {
            current = actions.front();
            actions.pop_front();
            actionEnd = now + std::chrono::milliseconds(current.milliseconds);
            if (!current.shot.empty())
                gpu::d3d12::RequestScreenshot(current.shot);
            if (!current.find.empty())
                RequestFind(current.find);
            if (current.scan)
            {
                RequestTimerScan(current.scan == 2);
                LOGF_INFO("remote: timer scan at frame {}", gpu::d3d12::GetFrameNumber());
            }
            LOGF_INFO("remote: buttons 0x{:04X} left {} right {} for {} ms at frame {}", current.buttons, current.left, current.right,
                current.milliseconds, gpu::d3d12::GetFrameNumber());
        }
    }

    auto stick = [](float angle, SHORT& x, SHORT& y)
        {
            float radians = angle * 3.14159265f / 180.0f;
            x = SHORT(-std::sin(radians) * 32767.0f);
            y = SHORT(std::cos(radians) * 32767.0f);
        };
    if (current.left >= 0.0f)
        stick(current.left, state.Gamepad.sThumbLX, state.Gamepad.sThumbLY);
    if (current.right >= 0.0f)
        stick(current.right, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY);
    state.Gamepad.wButtons |= current.buttons;
    if (current.leftTrigger)
        state.Gamepad.bLeftTrigger = 255;
    if (current.rightTrigger)
        state.Gamepad.bRightTrigger = 255;
    return true;
}

static bool GetControllerState(uint32_t user, XINPUT_STATE& state)
{
    bool connected = XInputGetState(user, &state) == ERROR_SUCCESS;
    if (!connected)
        memset(&state, 0, sizeof(state));

    if (user == 0 && AR_ENV("AR_REMOTE"))
    {
        RemoteControl(state);
        static std::atomic<uint32_t> packet;
        static std::atomic<uint64_t> lastState;
        uint64_t packed = state.Gamepad.wButtons | uint64_t(state.Gamepad.bLeftTrigger) << 16 | uint64_t(state.Gamepad.bRightTrigger) << 24 |
            uint64_t(uint16_t(state.Gamepad.sThumbLX)) << 32 | uint64_t(uint16_t(state.Gamepad.sThumbLY)) << 48;
        if (lastState.exchange(packed) != packed)
            packet++;
        state.dwPacketNumber = packet + (connected ? 0x10000 : 0);
        return true;
    }

    if (user == 0 && AR_ENV("AR_AUTO_PRESS"))
    {
        // The packet number changes with the state, which is how titles notice input.
        static std::atomic<uint32_t> packet;
        static std::atomic<uint16_t> lastButtons;
        float stickAngle;
        uint16_t autoButtons = AutoPressButtons(stickAngle);
        if (stickAngle >= 0.0f)
        {
            float radians = stickAngle * 3.14159265f / 180.0f;
            state.Gamepad.sThumbLX = SHORT(-std::sin(radians) * 32767.0f);
            state.Gamepad.sThumbLY = SHORT(std::cos(radians) * 32767.0f);
        }
        state.Gamepad.wButtons |= autoButtons & ~(AUTO_LEFT_TRIGGER | AUTO_HOLD);
        if (autoButtons & AUTO_LEFT_TRIGGER)
            state.Gamepad.bLeftTrigger = 255;
        // Directions also push the left stick, which some menus read instead of the D-pad.
        if (autoButtons & XINPUT_GAMEPAD_DPAD_UP)
            state.Gamepad.sThumbLY = 32767;
        if (autoButtons & XINPUT_GAMEPAD_DPAD_DOWN)
            state.Gamepad.sThumbLY = -32768;
        if (autoButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
            state.Gamepad.sThumbLX = 32767;
        if (autoButtons & XINPUT_GAMEPAD_DPAD_LEFT)
            state.Gamepad.sThumbLX = -32768;
        uint16_t buttonState = state.Gamepad.wButtons | (state.Gamepad.bLeftTrigger > 64 ? AUTO_LEFT_TRIGGER : 0);
        if (lastButtons.exchange(buttonState) != buttonState)
        {
            packet++;
            LOGF_INFO("auto press: buttons 0x{:04X} at frame {}", buttonState, gpu::d3d12::GetFrameNumber());
        }
        state.dwPacketNumber = packet;
        return true;
    }

    return connected;
}

static void UpdateKeystrokes(uint32_t user)
{
    XINPUT_STATE state;
    GetControllerState(user, state);

    auto& keys = g_keystrokes[user];
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < std::size(KEYS); i++)
    {
        bool down = IsKeyDown(state.Gamepad, KEYS[i]);
        bool wasDown = (keys.pressed >> i) & 1;
        if (down && !wasDown)
        {
            keys.queue.push_back({ KEYS[i].virtualKey, KEYSTROKE_KEYDOWN, user });
            keys.repeatTime[i] = now + std::chrono::milliseconds(400);
        }
        else if (!down && wasDown)
        {
            keys.queue.push_back({ KEYS[i].virtualKey, KEYSTROKE_KEYUP, user });
        }
        else if (down && now >= keys.repeatTime[i])
        {
            keys.queue.push_back({ KEYS[i].virtualKey, uint16_t(KEYSTROKE_KEYDOWN | KEYSTROKE_REPEAT), user });
            keys.repeatTime[i] = now + std::chrono::milliseconds(100);
        }

        keys.pressed = down ? (keys.pressed | (1ull << i)) : (keys.pressed & ~(1ull << i));
    }
}

static bool NextKeystroke(uint32_t user, Keystroke& event)
{
    std::lock_guard lock(g_keystrokeMutex);
    for (uint32_t i = 0; i < XUSER_MAX_COUNT; i++)
    {
        if ((user == 0xFF || user == i) && ((g_connectedMask.load() >> i) & 1))
            UpdateKeystrokes(i);
    }

    for (uint32_t i = 0; i < XUSER_MAX_COUNT; i++)
    {
        auto& queue = g_keystrokes[i].queue;
        if ((user == 0xFF || user == i) && !queue.empty())
        {
            event = queue.front();
            queue.pop_front();
            return true;
        }
    }

    return false;
}

namespace imports
{
    uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* capabilities)
    {
        StartConnectionMonitor();

        XINPUT_CAPABILITIES host;
        if ((userIndex & 0xFF) >= XUSER_MAX_COUNT || XInputGetCapabilities(userIndex & 0xFF, XINPUT_FLAG_GAMEPAD, &host) != ERROR_SUCCESS)
        {
            if ((userIndex & 0xFF) != 0 || !(AR_ENV("AR_AUTO_PRESS") || AR_ENV("AR_REMOTE")))
                return X_ERROR_DEVICE_NOT_CONNECTED;
            host = {};
            host.Type = XINPUT_DEVTYPE_GAMEPAD;
            host.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        }

        memset(capabilities, 0, sizeof(*capabilities));
        capabilities->Type = host.Type;
        capabilities->SubType = host.SubType;
        capabilities->Flags = ByteSwap(uint16_t(host.Flags));
        WriteGamepad(&capabilities->Gamepad, host.Gamepad);
        capabilities->Vibration.wLeftMotorSpeed = ByteSwap(uint16_t(host.Vibration.wLeftMotorSpeed));
        capabilities->Vibration.wRightMotorSpeed = ByteSwap(uint16_t(host.Vibration.wRightMotorSpeed));
        return X_ERROR_SUCCESS;
    }

    uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
    {
        StartConnectionMonitor();

        XINPUT_STATE host;
        if ((userIndex & 0xFF) >= XUSER_MAX_COUNT || !GetControllerState(userIndex & 0xFF, host))
            return X_ERROR_DEVICE_NOT_CONNECTED;

        state->dwPacketNumber = ByteSwap(uint32_t(host.dwPacketNumber));
        WriteGamepad(&state->Gamepad, host.Gamepad);
        return X_ERROR_SUCCESS;
    }

    uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
    {
        if ((userIndex & 0xFF) >= XUSER_MAX_COUNT)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        XINPUT_VIBRATION host;
        host.wLeftMotorSpeed = ByteSwap(vibration->wLeftMotorSpeed);
        host.wRightMotorSpeed = ByteSwap(vibration->wRightMotorSpeed);
        return XInputSetState(userIndex & 0xFF, &host) == ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    // X_INPUT_KEYSTROKE: VirtualKey, Unicode, Flags (big endian 16 bit), UserIndex, HidCode. A user index of 0xFF
    // reads from any controller and reports which one.
    uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, uint8_t* keystroke)
    {
        uint32_t user = userIndex != nullptr ? (userIndex->get() & 0xFF) : 0xFF;
        if (user != 0xFF && user >= XUSER_MAX_COUNT)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        Keystroke event;
        if (!NextKeystroke(user, event))
            return user == 0xFF || (g_connectedMask.load() >> user) & 1 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;

        *reinterpret_cast<be<uint16_t>*>(keystroke + 0) = event.virtualKey;
        *reinterpret_cast<be<uint16_t>*>(keystroke + 2) = 0;
        *reinterpret_cast<be<uint16_t>*>(keystroke + 4) = event.flags;
        keystroke[6] = uint8_t(event.user);
        keystroke[7] = 0;

        if (userIndex != nullptr)
            *userIndex = event.user;

        if (AR_ENV("AR_AUTO_PRESS"))
            LOGF_INFO("keystroke 0x{:04X} flags {} to user {} (requested 0x{:X})", event.virtualKey, event.flags, event.user, user);
        return X_ERROR_SUCCESS;
    }
}

AR_IMPORT(XamInputGetCapabilities);
AR_IMPORT(XamInputGetState);
AR_IMPORT(XamInputSetState);
AR_IMPORT(XamInputGetKeystrokeEx);
