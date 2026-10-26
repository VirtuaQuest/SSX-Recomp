// Controller input through XInput. Guest controller indices map directly to XInput user indices.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/xam.h>
#include <Xinput.h>

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
            g_connectedMask = QueryConnectedMask();
            LOGF_INFO("controllers connected: mask 0x{:X}", g_connectedMask.load());

            std::thread([]
                {
                    while (true)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        uint32_t mask = QueryConnectedMask();
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

namespace imports
{
    uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* capabilities)
    {
        StartConnectionMonitor();

        XINPUT_CAPABILITIES host;
        if ((userIndex & 0xFF) >= XUSER_MAX_COUNT || XInputGetCapabilities(userIndex & 0xFF, XINPUT_FLAG_GAMEPAD, &host) != ERROR_SUCCESS)
            return X_ERROR_DEVICE_NOT_CONNECTED;

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
        if ((userIndex & 0xFF) >= XUSER_MAX_COUNT || XInputGetState(userIndex & 0xFF, &host) != ERROR_SUCCESS)
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
        DWORD user = userIndex != nullptr ? (userIndex->get() & 0xFF) : XUSER_INDEX_ANY;
        if (user != XUSER_INDEX_ANY && user >= XUSER_MAX_COUNT)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        XINPUT_KEYSTROKE host;
        DWORD result = XInputGetKeystroke(user, 0, &host);
        if (result == ERROR_EMPTY)
            return X_ERROR_EMPTY;
        if (result != ERROR_SUCCESS)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        *reinterpret_cast<be<uint16_t>*>(keystroke + 0) = host.VirtualKey;
        *reinterpret_cast<be<uint16_t>*>(keystroke + 2) = uint16_t(host.Unicode);
        *reinterpret_cast<be<uint16_t>*>(keystroke + 4) = host.Flags;
        keystroke[6] = host.UserIndex;
        keystroke[7] = host.HidCode;

        if (userIndex != nullptr)
            *userIndex = host.UserIndex;

        return X_ERROR_SUCCESS;
    }
}

AR_IMPORT(XamInputGetCapabilities);
AR_IMPORT(XamInputGetState);
AR_IMPORT(XamInputSetState);
AR_IMPORT(XamInputGetKeystrokeEx);
