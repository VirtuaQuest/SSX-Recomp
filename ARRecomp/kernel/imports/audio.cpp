// Audio: the XAudio render driver and XMA contexts.
//
// There is no audio output yet. The render driver calls the title's audio client at the hardware rate (256
// samples at 48 kHz) and discards the submitted frames, which keeps the title's audio engine running. XMA
// contexts are allocated but nothing decodes them yet.
#include <stdafx.h>
#include <kernel/imports.h>
#include <cpu/guest_thread.h>

static std::atomic<uint32_t> g_clientCallback;
// The callback receives the address of a guest word holding the client's context, not the context itself.
static std::atomic<uint32_t> g_clientContext;

static void RunAudioDriver()
{
    auto next = std::chrono::steady_clock::now();
    while (true)
    {
        next += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(next);

        if (uint32_t callback = g_clientCallback)
            GuestToHostFunction<void>(callback, g_clientContext.load());
    }
}

namespace imports
{
    // The client is a pointer to { callback, context }.
    uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* client, be<uint32_t>* driver)
    {
        uint32_t context = g_runtimeHeap.AllocGuest(4);
        *GuestPtr<be<uint32_t>>(context) = client[1].get();
        g_clientContext = context;
        g_clientCallback = client[0];
        *driver = 0x41550000; // Driver handle, checked only for being nonzero.

        static std::once_flag once;
        std::call_once(once, []
            {
                ThreadStartParams params{};
                params.hostEntry = RunAudioDriver;
                params.name = "Audio driver";
                CreateGuestThread(params);
            });

        return X_STATUS_SUCCESS;
    }

    uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
    {
        g_clientCallback = 0;
        return X_STATUS_SUCCESS;
    }

    uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, uint32_t samples)
    {
        return X_STATUS_SUCCESS;
    }

    uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
    {
        *config = 0x00010001;
        return X_STATUS_SUCCESS;
    }

    uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, be<float>* volume)
    {
        *volume = 1.0f;
        return X_STATUS_SUCCESS;
    }

    // Ducking lowers game audio under voice chat, which is unavailable.
    uint32_t XAudioEnableDucker(uint32_t enable) { return X_STATUS_SUCCESS; }
    uint32_t XAudioGetDuckerLevel(be<float>* level) { if (level) *level = 1.0f; return X_STATUS_SUCCESS; }
    uint32_t XAudioGetDuckerThreshold(be<float>* threshold) { if (threshold) *threshold = 0.0f; return X_STATUS_SUCCESS; }
    uint32_t XAudioGetDuckerAttackTime(be<float>* time) { if (time) *time = 0.0f; return X_STATUS_SUCCESS; }
    uint32_t XAudioGetDuckerReleaseTime(be<float>* time) { if (time) *time = 0.0f; return X_STATUS_SUCCESS; }
    uint32_t XAudioGetDuckerHoldTime(be<float>* time) { if (time) *time = 0.0f; return X_STATUS_SUCCESS; }

    // XMA contexts live in physical memory, where the decoder hardware reads them.
    uint32_t XMACreateContext(be<uint32_t>* context)
    {
        *context = AllocatePhysicalMemory(0x40, 0x1000, PHYSICAL_VIEW_4K, 0x04, false);
        return *context != 0 ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
    }

    void XMAReleaseContext(uint32_t context)
    {
        FreePhysicalMemory(context);
    }

    uint32_t HidReadKeys(uint32_t unknown1, uint32_t unknown2, uint32_t unknown3)
    {
        return 0xC000009D; // STATUS_DEVICE_NOT_CONNECTED
    }
}

AR_IMPORT(XAudioRegisterRenderDriverClient);
AR_IMPORT(XAudioUnregisterRenderDriverClient);
AR_IMPORT(XAudioSubmitRenderDriverFrame);
AR_IMPORT(XAudioGetSpeakerConfig);
AR_IMPORT(XAudioGetVoiceCategoryVolume);
AR_IMPORT(XAudioEnableDucker);
AR_IMPORT(XAudioGetDuckerLevel);
AR_IMPORT(XAudioGetDuckerThreshold);
AR_IMPORT(XAudioGetDuckerAttackTime);
AR_IMPORT(XAudioGetDuckerReleaseTime);
AR_IMPORT(XAudioGetDuckerHoldTime);
AR_IMPORT(XMACreateContext);
AR_IMPORT(XMAReleaseContext);
AR_IMPORT(HidReadKeys);
