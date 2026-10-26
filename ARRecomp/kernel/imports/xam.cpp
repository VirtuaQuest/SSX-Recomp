// XAM: users, content (save data), input, notifications, system UI and title loader services.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/vfs.h>
#include <kernel/xex_loader.h>
#include <xex.h>

// ---- Overlapped requests ----

// XOVERLAPPED: InternalLow (result), InternalHigh (length), InternalContext, hEvent, pCompletionRoutine,
// dwCompletionContext, dwExtendedError.
void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length, uint32_t extendedError)
{
    if (overlapped == 0)
        return;

    auto* data = GuestPtr<be<uint32_t>>(overlapped);
    data[0] = result;
    data[1] = length;
    data[6] = extendedError != 0 ? extendedError : result;

    if (data[4] != 0)
        LOGF_WARNING("overlapped completion routine 0x{:08X} is not called", data[4].get());

    if (uint32_t event = data[3])
    {
        if (auto object = LookupHandle(event); object && object->guestObject != 0)
            SetDispatcherEvent(object->Header());
    }
}

// Finishes a request that may be asynchronous: completes the overlapped and returns ERROR_IO_PENDING, or returns
// the result directly when there is no overlapped.
static uint32_t FinishRequest(uint32_t overlapped, uint32_t result, uint32_t length = 0)
{
    if (overlapped == 0)
        return result;

    CompleteOverlapped(overlapped, result, length);
    return X_ERROR_IO_PENDING;
}

// ---- Enumerators ----

struct EnumeratorObject : KernelObject
{
    uint32_t itemSize{};
    std::vector<std::vector<uint8_t>> items;
    size_t position{};

    EnumeratorObject() : KernelObject(ObjectType::Other) {}
};

// ---- Notification listeners ----

struct ListenerObject : KernelObject
{
    uint64_t mask{};
    std::mutex mutex;
    std::vector<std::pair<uint32_t, uint32_t>> queue;

    ListenerObject() : KernelObject(ObjectType::Event) {}

    void Post(uint32_t id, uint32_t parameter)
    {
        // The notification area is in bits 25-30 of the id, the listener mask selects areas.
        uint32_t area = (id >> 25) & 0x3F;
        if (!(mask & (1ull << area)))
            return;

        std::lock_guard lock(mutex);
        queue.emplace_back(id, parameter);
        SetDispatcherEvent(Header());
    }
};

static std::mutex g_listenersMutex;
static std::vector<std::weak_ptr<ListenerObject>> g_listeners;

constexpr uint32_t XN_SYS_UI = 0x00000009;
constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
constexpr uint32_t XN_SYS_INPUTDEVICESCHANGED = 0x00000012;
constexpr uint32_t XN_LIVE_CONNECTIONCHANGED = 0x02000001;

static void PostNotification(uint32_t id, uint32_t parameter)
{
    std::lock_guard lock(g_listenersMutex);
    for (const auto& weak : g_listeners)
    {
        if (auto listener = weak.lock())
            listener->Post(id, parameter);
    }
}

// ---- Content ----

// XCONTENT_DATA: DeviceID, dwContentType, szDisplayName (128 wide chars), szFileName (42 chars), padding.
struct XContentData
{
    be<uint32_t> deviceId;
    be<uint32_t> contentType;
    be<uint16_t> displayName[128];
    char fileName[42];
    uint8_t padding[2];
};

static_assert(sizeof(XContentData) == 0x134);

constexpr uint32_t CONTENT_DEVICE_ID = 1; // The hard drive.

static std::filesystem::path ContentPath(uint32_t contentType, std::string_view fileName)
{
    return vfs::UserPath() / "content" / fmt::format("{:08X}", contentType) / std::string(fileName);
}

static uint64_t g_userXuid = 0xE000000000000001ull;
static const char* g_userName = "Player";

namespace imports
{
    // ---- Loader ----

    static std::vector<uint8_t> g_launchData;

    uint32_t XamLoaderGetLaunchDataSize(be<uint32_t>* size)
    {
        *size = uint32_t(g_launchData.size());
        return g_launchData.empty() ? X_ERROR_NOT_FOUND : X_ERROR_SUCCESS;
    }

    uint32_t XamLoaderGetLaunchData(uint8_t* buffer, uint32_t bufferSize)
    {
        if (g_launchData.empty())
            return X_ERROR_NOT_FOUND;

        memcpy(buffer, g_launchData.data(), std::min<size_t>(bufferSize, g_launchData.size()));
        return X_ERROR_SUCCESS;
    }

    uint32_t XamLoaderSetLaunchData(const uint8_t* data, uint32_t size)
    {
        g_launchData.assign(data, data + size);
        return X_ERROR_SUCCESS;
    }

    void XamLoaderLaunchTitle(const char* name, uint32_t flags)
    {
        LOGF_INFO("title requested launching {}, exiting", name != nullptr ? name : "(null)");
        std::exit(0);
    }

    void XamLoaderTerminateTitle()
    {
        LOGF_INFO("title terminated itself");
        std::exit(0);
    }

    // ---- System information ----

    uint32_t XamGetExecutionId(be<uint32_t>* info)
    {
        *info = FindXexHeaderField(g_module.xexHeader, XEX_HEADER_EXECUTION_INFO);
        return X_STATUS_SUCCESS;
    }

    uint32_t XamGetSystemVersion()
    {
        return 0x20525400; // 2.0.21076.0, the kernel version the executable was linked against.
    }

    uint32_t XGetAVPack()
    {
        return 8; // HDMI
    }

    uint32_t XGetGameRegion()
    {
        return 0xFFFF; // All regions.
    }

    uint32_t XGetLanguage()
    {
        return 1; // English
    }

    void XGetVideoMode(XVIDEO_MODE* mode)
    {
        // Based on Unleashed Recompiled's VdQueryVideoMode.
        memset(mode, 0, sizeof(*mode));
        mode->DisplayWidth = 1280;
        mode->DisplayHeight = 720;
        mode->IsInterlaced = false;
        mode->IsWidescreen = true;
        mode->IsHighDefinition = true;
        mode->RefreshRate = 0x42700000; // 60.0f
        mode->VideoStandard = 1;
        mode->Unknown4A = 0x4A;
        mode->Unknown01 = 0x01;
    }

    uint32_t XamResetInactivity(uint32_t unknown)
    {
        return 0;
    }

    uint32_t XamEnableInactivityProcessing(uint32_t unknown, uint32_t enable)
    {
        return 0;
    }

    uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* address)
    {
        *address = g_runtimeHeap.AllocGuest(size);
        return *address != 0 ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
    }

    uint32_t XamFree(uint32_t address)
    {
        g_runtimeHeap.FreeGuest(address);
        return X_ERROR_SUCCESS;
    }

    // ---- Messaging with system applications ----

    uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, uint32_t parameter1, uint32_t parameter2)
    {
        LOGF_WARNING("unhandled message 0x{:08X} to app 0x{:08X}", message, app);
        return X_ERROR_NOT_FOUND;
    }

    uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t bufferLength)
    {
        LOGF_WARNING("unhandled I/O request 0x{:08X} to app 0x{:08X}", message, app);
        return FinishRequest(overlapped, X_ERROR_NOT_FOUND);
    }

    uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t bufferLength, uint32_t unknown)
    {
        LOGF_WARNING("unhandled I/O request 0x{:08X} to app 0x{:08X}", message, app);
        return FinishRequest(overlapped, X_ERROR_NOT_FOUND);
    }

    uint32_t XMsgCancelIORequest(uint32_t overlapped, uint32_t wait)
    {
        return X_STATUS_SUCCESS;
    }

    // ---- Users ----

    // One local profile is signed in on the first controller; it is never signed in to Xbox Live.
    uint32_t XamUserGetSigninState(uint32_t userIndex)
    {
        return userIndex == 0 ? 1 : 0; // eXUserSigninState_SignedInLocally
    }

    uint32_t XamUserGetSigninInfo(uint32_t userIndex, uint32_t flags, XUSER_SIGNIN_INFO* info)
    {
        memset(info, 0, sizeof(*info));
        if (userIndex != 0)
            return X_ERROR_NO_SUCH_USER;

        info->xuid = g_userXuid;
        info->SigninState = 1;
        strcpy(info->Name, g_userName);
        return X_ERROR_SUCCESS;
    }

    uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t typeMask, be<uint64_t>* xuid)
    {
        if (userIndex != 0)
            return X_ERROR_NO_SUCH_USER;

        *xuid = g_userXuid;
        return X_ERROR_SUCCESS;
    }

    uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t bufferSize)
    {
        if (userIndex != 0)
            return X_ERROR_NO_SUCH_USER;

        if (bufferSize > 0)
        {
            strncpy(buffer, g_userName, bufferSize - 1);
            buffer[bufferSize - 1] = '\0';
        }

        return X_ERROR_SUCCESS;
    }

    uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t privilege, be<uint32_t>* result)
    {
        if (userIndex != 0)
            return X_ERROR_NO_SUCH_USER;

        // Offline profile: no Live privileges such as multiplayer sessions or communication.
        *result = 0;
        return X_ERROR_SUCCESS;
    }

    uint32_t XamUserAreUsersFriends(uint32_t userIndex, be<uint64_t>* xuids, uint32_t count, be<uint32_t>* results, uint32_t overlapped)
    {
        for (uint32_t i = 0; i < count && results != nullptr; i++)
            results[i] = 0;

        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamUserGetDeviceContext(uint32_t userIndex, uint32_t unknown, be<uint32_t>* context)
    {
        if (userIndex != 0)
            return X_ERROR_NO_SUCH_USER;

        if (context != nullptr)
            *context = 0;

        return X_ERROR_SUCCESS;
    }

    // X_USER_READ_PROFILE_SETTINGS: setting count and pointer to X_USER_PROFILE_SETTING entries (0x28 bytes:
    // source, user index or XUID at +0x08, setting id at +0x10, X_USER_DATA at +0x18).
    uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, uint32_t xuids,
        uint32_t settingCount, be<uint32_t>* settingIds, be<uint32_t>* bufferSize, uint8_t* buffer, uint32_t overlapped)
    {
        uint32_t users = std::max<uint32_t>(xuidCount, 1);
        uint32_t required = 8 + users * settingCount * 0x28;

        if (buffer == nullptr || *bufferSize < required)
        {
            *bufferSize = required;
            return FinishRequest(overlapped, X_ERROR_INSUFFICIENT_BUFFER);
        }

        memset(buffer, 0, required);
        uint32_t settings = GuestAddress(buffer) + 8;
        *reinterpret_cast<be<uint32_t>*>(buffer + 0) = users * settingCount;
        *reinterpret_cast<be<uint32_t>*>(buffer + 4) = settings;

        // Every setting is reported as not set, so the title uses its defaults.
        for (uint32_t user = 0; user < users; user++)
        {
            for (uint32_t i = 0; i < settingCount; i++)
            {
                uint8_t* entry = buffer + 8 + (user * settingCount + i) * 0x28;
                uint32_t id = settingIds[i];
                *reinterpret_cast<be<uint32_t>*>(entry + 0x00) = 0;
                *reinterpret_cast<be<uint32_t>*>(entry + 0x08) = userIndex;
                *reinterpret_cast<be<uint32_t>*>(entry + 0x10) = id;
                entry[0x18] = uint8_t(id >> 28); // Data type.
            }
        }

        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    static uint32_t CreateEmptyEnumerator(uint32_t itemSize, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        auto enumerator = std::make_shared<EnumeratorObject>();
        enumerator->itemSize = itemSize;
        if (bufferSize != nullptr)
            *bufferSize = itemSize;

        *handle = CreateHandle(enumerator);
        return X_ERROR_SUCCESS;
    }

    uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t flags,
        uint32_t offset, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        return CreateEmptyEnumerator(0x1C, bufferSize, handle);
    }

    uint32_t XamUserCreateStatsEnumerator(uint32_t titleId, uint32_t unknown1, uint32_t unknown2, uint32_t unknown3,
        uint32_t unknown4, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        return CreateEmptyEnumerator(0x10, bufferSize, handle);
    }

    uint32_t XamEnumerate(uint32_t handle, uint32_t flags, uint8_t* buffer, uint32_t bufferSize, be<uint32_t>* itemsReturned, uint32_t overlapped)
    {
        auto enumerator = std::dynamic_pointer_cast<EnumeratorObject>(LookupHandle(handle));
        if (!enumerator)
            return X_ERROR_INVALID_HANDLE;

        uint32_t count = 0;
        while (enumerator->position < enumerator->items.size() && (count + 1) * enumerator->itemSize <= bufferSize)
        {
            const auto& item = enumerator->items[enumerator->position++];
            memcpy(buffer + count * enumerator->itemSize, item.data(), std::min<size_t>(item.size(), enumerator->itemSize));
            count++;
        }

        if (itemsReturned != nullptr)
            *itemsReturned = count;

        uint32_t result = count != 0 ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
        if (overlapped != 0)
        {
            CompleteOverlapped(overlapped, result, count);
            return X_ERROR_IO_PENDING;
        }

        return result;
    }

    // ---- Input ----

    // Until host input is connected, the first controller is reported as present and idle. The XAMINPUT structures
    // in xbox.h mirror the host XInput layout, so multi-byte fields are byte swapped by hand.
    uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* capabilities)
    {
        if ((userIndex & 0xFF) != 0)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        memset(capabilities, 0, sizeof(*capabilities));
        capabilities->Type = XAMINPUT_DEVTYPE_GAMEPAD;
        capabilities->SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD;
        capabilities->Gamepad.wButtons = ByteSwap(uint16_t(0xF3FF));
        capabilities->Gamepad.bLeftTrigger = 0xFF;
        capabilities->Gamepad.bRightTrigger = 0xFF;
        capabilities->Gamepad.sThumbLX = int16_t(ByteSwap(uint16_t(0xFFC0)));
        capabilities->Gamepad.sThumbLY = int16_t(ByteSwap(uint16_t(0xFFC0)));
        capabilities->Gamepad.sThumbRX = int16_t(ByteSwap(uint16_t(0xFFC0)));
        capabilities->Gamepad.sThumbRY = int16_t(ByteSwap(uint16_t(0xFFC0)));
        capabilities->Vibration.wLeftMotorSpeed = 0xFFFF;
        capabilities->Vibration.wRightMotorSpeed = 0xFFFF;
        return X_ERROR_SUCCESS;
    }

    static std::atomic<uint32_t> g_inputPacket;

    uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
    {
        if ((userIndex & 0xFF) != 0)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        memset(state, 0, sizeof(*state));
        state->dwPacketNumber = ByteSwap(g_inputPacket.load());
        return X_ERROR_SUCCESS;
    }

    uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
    {
        return (userIndex & 0xFF) == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, uint8_t* keystroke)
    {
        return X_ERROR_EMPTY;
    }

    // ---- Content ----

    uint32_t XamContentGetDeviceData(uint32_t deviceId, XDEVICE_DATA* data)
    {
        if (deviceId != CONTENT_DEVICE_ID)
            return X_ERROR_DEVICE_NOT_CONNECTED;

        memset(data, 0, sizeof(*data));
        data->DeviceID = CONTENT_DEVICE_ID;
        data->DeviceType = XCONTENTDEVICETYPE_HDD;
        data->ulDeviceBytes = 20ull * 1024 * 1024 * 1024;
        data->ulDeviceFreeBytes = 16ull * 1024 * 1024 * 1024;

        const char16_t* name = u"Hard Drive";
        for (size_t i = 0; name[i] != 0; i++)
            data->wszName[i] = uint16_t(name[i]);

        return X_ERROR_SUCCESS;
    }

    uint32_t XamContentCreateEnumerator(uint32_t userIndex, uint32_t deviceId, uint32_t contentType, uint32_t contentFlags,
        uint32_t itemsPerEnumerate, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        auto enumerator = std::make_shared<EnumeratorObject>();
        enumerator->itemSize = sizeof(XContentData);

        if (deviceId == 0 || deviceId == CONTENT_DEVICE_ID)
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(ContentPath(contentType, ""), ec))
            {
                if (!entry.is_directory())
                    continue;

                std::vector<uint8_t> item(sizeof(XContentData));
                auto* data = reinterpret_cast<XContentData*>(item.data());
                std::string fileName = entry.path().filename().string();
                data->deviceId = CONTENT_DEVICE_ID;
                data->contentType = contentType;
                strncpy(data->fileName, fileName.c_str(), sizeof(data->fileName) - 1);
                for (size_t i = 0; i < fileName.size() && i < 127; i++)
                    data->displayName[i] = uint16_t(uint8_t(fileName[i]));

                enumerator->items.push_back(std::move(item));
            }
        }

        if (bufferSize != nullptr)
            *bufferSize = std::max<uint32_t>(itemsPerEnumerate, 1) * sizeof(XContentData);

        *handle = CreateHandle(enumerator);
        return X_ERROR_SUCCESS;
    }

    // Content is mounted as a device named after the root ("save:" for root "save").
    uint32_t XamContentCreateEx(uint32_t userIndex, const char* rootName, XContentData* contentData, uint32_t flags,
        be<uint32_t>* disposition, be<uint32_t>* licenseMask, uint32_t cacheSize, uint64_t contentSize, uint32_t overlapped)
    {
        std::string fileName(contentData->fileName, strnlen(contentData->fileName, sizeof(contentData->fileName)));
        auto path = ContentPath(contentData->contentType, fileName);

        std::error_code ec;
        bool exists = std::filesystem::is_directory(path, ec);
        uint32_t mode = flags & 0xF;

        // Modes follow the Win32 creation dispositions: CREATE_NEW 1, CREATE_ALWAYS 2, OPEN_EXISTING 3,
        // OPEN_ALWAYS 4, TRUNCATE_EXISTING 5.
        uint32_t result = X_ERROR_SUCCESS;
        if (mode == 1 && exists)
            result = X_ERROR_ALREADY_EXISTS;
        else if ((mode == 3 || mode == 5) && !exists)
            result = X_ERROR_PATH_NOT_FOUND;

        if (result == X_ERROR_SUCCESS)
        {
            if (mode == 2 || mode == 5)
                std::filesystem::remove_all(path, ec);

            vfs::MapRoot(std::string(rootName) + ":", path);
            if (disposition != nullptr)
                *disposition = exists && mode != 2 && mode != 5 ? 2 : 1; // XCONTENT_EXISTING or XCONTENT_NEW
            if (licenseMask != nullptr)
                *licenseMask = 0;
        }

        LOGF_INFO("content {} (type 0x{:X}) as {}: result {}", fileName, contentData->contentType.get(), rootName, result);
        return FinishRequest(overlapped, result);
    }

    uint32_t XamContentClose(const char* rootName, uint32_t overlapped)
    {
        vfs::UnmapRoot(std::string(rootName) + ":");
        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamContentDelete(uint32_t userIndex, XContentData* contentData, uint32_t overlapped)
    {
        std::string fileName(contentData->fileName, strnlen(contentData->fileName, sizeof(contentData->fileName)));
        std::error_code ec;
        std::filesystem::remove_all(ContentPath(contentData->contentType, fileName), ec);
        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    // ---- System UI ----

    static std::string ReadWideString(const be<uint16_t>* text)
    {
        std::string result;
        for (; text != nullptr && text->get() != 0; text++)
            result += text->get() < 0x80 ? char(text->get()) : '?';

        return result;
    }

    uint32_t XamShowSigninUI(uint32_t unknown, uint32_t mask)
    {
        PostNotification(XN_SYS_UI, 1);
        PostNotification(XN_SYS_UI, 0);
        return X_ERROR_SUCCESS;
    }

    uint32_t XamShowDeviceSelectorUI(uint32_t userIndex, uint32_t contentType, uint32_t contentFlags, uint64_t totalRequested,
        be<uint32_t>* deviceId, uint32_t overlapped)
    {
        *deviceId = CONTENT_DEVICE_ID;
        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamShowMessageBoxUI(uint32_t userIndex, be<uint16_t>* title, be<uint16_t>* text, uint32_t buttonCount,
        uint32_t buttons, uint32_t activeButton, uint32_t flags, be<uint32_t>* result, uint32_t overlapped)
    {
        LOGF_WARNING("message box \"{}\": {}", ReadWideString(title), ReadWideString(text));
        *result = activeButton;
        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamShowMessageBoxUIEx(uint32_t userIndex, be<uint16_t>* title, be<uint16_t>* text, uint32_t buttonCount,
        uint32_t buttons, uint32_t activeButton, uint32_t flags, uint32_t unknown, be<uint32_t>* result, uint32_t overlapped)
    {
        LOGF_WARNING("message box \"{}\": {}", ReadWideString(title), ReadWideString(text));
        *result = activeButton;
        return FinishRequest(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamShowDirtyDiscErrorUI(uint32_t userIndex)
    {
        LOGF_ERROR("the title reported a disc read error");
        std::exit(1);
    }

    uint32_t XamShowGameInviteUI(uint32_t userIndex, uint32_t unknown1, uint32_t unknown2, uint32_t unknown3)
    {
        return X_ERROR_FUNCTION_FAILED;
    }

    uint32_t XamShowGamerCardUIForXUID(uint32_t userIndex, uint64_t xuid)
    {
        return X_ERROR_FUNCTION_FAILED;
    }

    uint32_t XamShowFriendsUI(uint32_t userIndex)
    {
        return X_ERROR_FUNCTION_FAILED;
    }

    // ---- Notifications ----

    uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t maxVersion)
    {
        auto listener = CreateGuestBackedObject<ListenerObject>(sizeof(XDISPATCHER_HEADER));
        listener->Header()->Type = DISPATCHER_NOTIFICATION_EVENT;
        listener->Header()->Size = sizeof(XDISPATCHER_HEADER) / 4;
        listener->mask = mask;

        {
            std::lock_guard lock(g_listenersMutex);
            g_listeners.push_back(listener);
        }

        // Titles expect the current sign-in and input device state when they start listening.
        listener->Post(XN_SYS_SIGNINCHANGED, 1);
        listener->Post(XN_SYS_INPUTDEVICESCHANGED, 0);
        listener->Post(XN_LIVE_CONNECTIONCHANGED, 0x80151001); // XONLINE_E_LOGON_CANNOT_ACCESS_SERVICE

        return CreateHandle(listener);
    }

    uint32_t XNotifyGetNext(uint32_t handle, uint32_t matchId, be<uint32_t>* id, be<uint32_t>* parameter)
    {
        auto listener = std::dynamic_pointer_cast<ListenerObject>(LookupHandle(handle));
        if (!listener)
            return false;

        std::lock_guard lock(listener->mutex);
        for (size_t i = 0; i < listener->queue.size(); i++)
        {
            auto [notificationId, notificationParameter] = listener->queue[i];
            if (matchId != 0 && notificationId != matchId)
                continue;

            listener->queue.erase(listener->queue.begin() + i);
            if (listener->queue.empty())
                ResetDispatcherEvent(listener->Header());

            *id = notificationId;
            if (parameter != nullptr)
                *parameter = notificationParameter;

            return true;
        }

        return false;
    }

    void XNotifyPositionUI(uint32_t position)
    {
    }

    // ---- Voice and sessions, unavailable offline ----

    uint32_t XamVoiceCreate(uint32_t unknown1, uint32_t unknown2, be<uint32_t>* voice)
    {
        if (voice != nullptr)
            *voice = 0;

        return X_ERROR_ACCESS_DENIED;
    }

    uint32_t XamVoiceClose(uint32_t voice)
    {
        return X_ERROR_SUCCESS;
    }

    uint32_t XamVoiceHeadsetPresent(uint32_t voice)
    {
        return false;
    }

    uint32_t XamVoiceSubmitPacket(uint32_t voice, uint32_t size, uint32_t data)
    {
        return X_ERROR_SUCCESS;
    }

    uint32_t XamVoiceIsActiveProcess()
    {
        return false;
    }

    uint32_t XamSessionCreateHandle(be<uint32_t>* handle)
    {
        LOGF_WARNING("Xbox Live sessions are not available");
        return X_ERROR_FUNCTION_FAILED;
    }

    uint32_t XamSessionRefObjByHandle(uint32_t handle, be<uint32_t>* object)
    {
        return X_ERROR_INVALID_HANDLE;
    }
}

AR_IMPORT(XamLoaderGetLaunchDataSize);
AR_IMPORT(XamLoaderGetLaunchData);
AR_IMPORT(XamLoaderSetLaunchData);
AR_IMPORT(XamLoaderLaunchTitle);
AR_IMPORT(XamLoaderTerminateTitle);
AR_IMPORT(XamGetExecutionId);
AR_IMPORT(XamGetSystemVersion);
AR_IMPORT(XGetAVPack);
AR_IMPORT(XGetGameRegion);
AR_IMPORT(XGetLanguage);
AR_IMPORT(XGetVideoMode);
AR_IMPORT(XamResetInactivity);
AR_IMPORT(XamEnableInactivityProcessing);
AR_IMPORT(XamAlloc);
AR_IMPORT(XamFree);
AR_IMPORT(XMsgInProcessCall);
AR_IMPORT(XMsgStartIORequest);
AR_IMPORT(XMsgStartIORequestEx);
AR_IMPORT(XMsgCancelIORequest);
AR_IMPORT(XamUserGetSigninState);
AR_IMPORT(XamUserGetSigninInfo);
AR_IMPORT(XamUserGetXUID);
AR_IMPORT(XamUserGetName);
AR_IMPORT(XamUserCheckPrivilege);
AR_IMPORT(XamUserAreUsersFriends);
AR_IMPORT(XamUserGetDeviceContext);
AR_IMPORT(XamUserReadProfileSettings);
AR_IMPORT(XamUserCreateAchievementEnumerator);
AR_IMPORT(XamUserCreateStatsEnumerator);
AR_IMPORT(XamEnumerate);
AR_IMPORT(XamInputGetCapabilities);
AR_IMPORT(XamInputGetState);
AR_IMPORT(XamInputSetState);
AR_IMPORT(XamInputGetKeystrokeEx);
AR_IMPORT(XamContentGetDeviceData);
AR_IMPORT(XamContentCreateEnumerator);
AR_IMPORT(XamContentCreateEx);
AR_IMPORT(XamContentClose);
AR_IMPORT(XamContentDelete);
AR_IMPORT(XamShowSigninUI);
AR_IMPORT(XamShowDeviceSelectorUI);
AR_IMPORT(XamShowMessageBoxUI);
AR_IMPORT(XamShowMessageBoxUIEx);
AR_IMPORT(XamShowDirtyDiscErrorUI);
AR_IMPORT(XamShowGameInviteUI);
AR_IMPORT(XamShowGamerCardUIForXUID);
AR_IMPORT(XamShowFriendsUI);
AR_IMPORT(XamNotifyCreateListener);
AR_IMPORT(XNotifyGetNext);
AR_IMPORT(XNotifyPositionUI);
AR_IMPORT(XamVoiceCreate);
AR_IMPORT(XamVoiceClose);
AR_IMPORT(XamVoiceHeadsetPresent);
AR_IMPORT(XamVoiceSubmitPacket);
AR_IMPORT(XamVoiceIsActiveProcess);
AR_IMPORT(XamSessionCreateHandle);
AR_IMPORT(XamSessionRefObjByHandle);
