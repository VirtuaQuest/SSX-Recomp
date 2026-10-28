// Controller input through XInput. Guest controller indices map directly to XInput user indices.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/xam.h>
#include <Xinput.h>
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
            g_connectedMask = QueryConnectedMask() | (getenv("AR_AUTO_PRESS") ? 1u : 0u);
            LOGF_INFO("controllers connected: mask 0x{:X}", g_connectedMask.load());

            std::thread([]
                {
                    while (true)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        uint32_t mask = QueryConnectedMask() | (getenv("AR_AUTO_PRESS") ? 1u : 0u);
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

// Debugging without a controller (AR_AUTO_PRESS=<seconds>): controller 0 taps a button every that many seconds,
// alternating A and Start.
static uint16_t AutoPressButtons()
{
    static const int period = getenv("AR_AUTO_PRESS") ? atoi(getenv("AR_AUTO_PRESS")) : 0;
    static const auto start = std::chrono::steady_clock::now();
    if (period <= 0)
        return 0;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed < period * 1000 || elapsed % (period * 1000) >= 250)
        return 0;

    // AR_AUTO_SEQUENCE: comma separated buttons (A, B, X, Y, START, BACK, UP, DOWN, LEFT, RIGHT, LB, RB) pressed in
    // order instead (WAIT skips a turn); after the last one, A repeats, or nothing when the list ends with WAIT.
    static const std::vector<uint16_t> sequence = []
        {
            std::vector<uint16_t> buttons;
            static const std::pair<const char*, uint16_t> names[] = { { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B },
                { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y }, { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK },
                { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT },
                { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER } };
            if (const char* text = getenv("AR_AUTO_SEQUENCE"))
            {
                std::string list = text;
                size_t start = 0;
                while (start <= list.size())
                {
                    size_t end = list.find(',', start);
                    std::string name = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    if (name == "WAIT")
                        buttons.push_back(0);
                    for (auto& [buttonName, button] : names)
                    {
                        if (name == buttonName)
                            buttons.push_back(button);
                    }
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
            return buttons;
        }();

    size_t step = size_t(elapsed / (period * 1000)) - 1;
    if (!sequence.empty())
        return step < sequence.size() ? sequence[step] : sequence.back() == 0 ? 0 : XINPUT_GAMEPAD_A;

    return (elapsed / (period * 1000)) % 2 ? XINPUT_GAMEPAD_A : XINPUT_GAMEPAD_START;
}

static bool GetControllerState(uint32_t user, XINPUT_STATE& state)
{
    bool connected = XInputGetState(user, &state) == ERROR_SUCCESS;
    if (!connected)
        memset(&state, 0, sizeof(state));

    if (user == 0 && getenv("AR_AUTO_PRESS"))
    {
        // The packet number changes with the state, which is how titles notice input.
        static std::atomic<uint32_t> packet;
        static std::atomic<uint16_t> lastButtons;
        uint16_t autoButtons = AutoPressButtons();
        state.Gamepad.wButtons |= autoButtons;
        // Directions also push the left stick, which some menus read instead of the D-pad.
        if (autoButtons & XINPUT_GAMEPAD_DPAD_UP)
            state.Gamepad.sThumbLY = 32767;
        if (autoButtons & XINPUT_GAMEPAD_DPAD_DOWN)
            state.Gamepad.sThumbLY = -32768;
        if (autoButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
            state.Gamepad.sThumbLX = 32767;
        if (autoButtons & XINPUT_GAMEPAD_DPAD_LEFT)
            state.Gamepad.sThumbLX = -32768;
        if (lastButtons.exchange(state.Gamepad.wButtons) != state.Gamepad.wButtons)
        {
            packet++;
            LOGF_INFO("auto press: buttons 0x{:04X} at frame {}", state.Gamepad.wButtons, gpu::d3d12::GetFrameNumber());
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
            if ((userIndex & 0xFF) != 0 || !getenv("AR_AUTO_PRESS"))
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

        if (getenv("AR_AUTO_PRESS"))
            LOGF_INFO("keystroke 0x{:04X} flags {} to user {} (requested 0x{:X})", event.virtualKey, event.flags, event.user, user);
        return X_ERROR_SUCCESS;
    }
}

AR_IMPORT(XamInputGetCapabilities);
AR_IMPORT(XamInputGetState);
AR_IMPORT(XamInputSetState);
AR_IMPORT(XamInputGetKeystrokeEx);
