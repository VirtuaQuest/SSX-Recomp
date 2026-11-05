// Audio: the XAudio render driver and XMA contexts.
//
// The title mixes its audio in software and hands the render driver frames of 256 samples for 6 channels
// (planar, big endian float, 48 kHz). The driver calls the title's audio client whenever the host device needs
// more audio, which paces the title's mixer like the console's audio hardware, and plays the frames through
// XAudio2, downmixed to stereo. Based on Unleashed Recompiled's apu/driver/sdl2_driver.cpp (GPL-3.0).
//
// XMA contexts (hardware decoded compressed audio) are allocated, but nothing decodes them yet.
#include <stdafx.h>
#include <kernel/imports.h>
#include <cpu/guest_thread.h>
#include <kernel/keyboard_mouse.h>
#include <xaudio2.h>

constexpr uint32_t AUDIO_SAMPLE_RATE = 48000;
constexpr uint32_t AUDIO_CHANNELS = 6;
constexpr uint32_t AUDIO_FRAME_SAMPLES = 256;
constexpr uint32_t OUTPUT_CHANNELS = 2;

// Frames queued on the host device before the title is asked for more: about 21 ms of latency.
constexpr uint32_t MAX_QUEUED_FRAMES = 4;
// Buffers in flight are taken from a ring larger than the queue, so a buffer is never reused while queued.
constexpr uint32_t BUFFER_RING_SIZE = 16;

// The title can have more than one audio engine at a time: multiplayer starts a second for voices, and each registers as a client. Every one needs its ticks, or
// its voices never finish stopping and scene shutdown waits for them forever. What they hand back is mixed.
constexpr uint32_t MAX_CLIENTS = 8;
constexpr uint32_t DRIVER_HANDLE = 0x41550000;

struct AudioClient
{
    std::atomic<uint32_t> callback;
    // The callback receives the address of a guest word holding the client's context, not the context itself.
    std::atomic<uint32_t> context;
};

static AudioClient g_clients[MAX_CLIENTS];

class AudioOutput : public IXAudio2VoiceCallback
{
public:
    bool Initialize()
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return false;

        if (FAILED(XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
            return false;

        if (FAILED(m_xaudio->CreateMasteringVoice(&m_master, OUTPUT_CHANNELS, AUDIO_SAMPLE_RATE)))
            return false;

        if (getenv("AR_MAKE_ICON")) // --make-icon runs unheard.
            m_master->SetVolume(0.0f);

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = OUTPUT_CHANNELS;
        format.nSamplesPerSec = AUDIO_SAMPLE_RATE;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        if (FAILED(m_xaudio->CreateSourceVoice(&m_source, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this)))
            return false;

        m_bufferDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_source->Start();
        return true;
    }

    uint32_t QueuedFrames()
    {
        XAUDIO2_VOICE_STATE state;
        m_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued;
    }

    void WaitForBuffer()
    {
        WaitForSingleObject(m_bufferDone, 20);
    }

    // Downmix weights per source channel (front left/right, center, LFE, back left/right), as in Unleashed Recompiled.
    void Add(const be<float>* samples)
    {
        std::lock_guard lock(m_mixMutex);
        if (!m_mixUsed)
            m_mix.fill(0.0f);
        m_mixUsed = true;
        for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++)
        {
            float frontLeft = samples[0 * AUDIO_FRAME_SAMPLES + i];
            float frontRight = samples[1 * AUDIO_FRAME_SAMPLES + i];
            float center = samples[2 * AUDIO_FRAME_SAMPLES + i];
            float backLeft = samples[4 * AUDIO_FRAME_SAMPLES + i];
            float backRight = samples[5 * AUDIO_FRAME_SAMPLES + i];
            m_mix[i * 2 + 0] += frontLeft + center * 0.75f + backLeft;
            m_mix[i * 2 + 1] += frontRight + center * 0.75f + backRight;
        }
    }

    // What the clients handed back since the last time, to the device. False if they haven't handed back anything.
    bool Send()
    {
        std::lock_guard lock(m_mixMutex);
        if (!m_mixUsed)
            return false;
        m_mixUsed = false;

        auto& buffer = m_buffers[m_nextBuffer];
        m_nextBuffer = (m_nextBuffer + 1) % BUFFER_RING_SIZE;
        bool skipping = kbm::MovieSkipping();

        for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++)
        {
            // Going quiet quickly while a cutscene is being skipped.
            m_gain = skipping ? std::max(m_gain - 1.0f / (AUDIO_SAMPLE_RATE * 0.15f), 0.0f) : std::min(m_gain + 1.0f / (AUDIO_SAMPLE_RATE * 0.05f), 1.0f);
            buffer[i * 2 + 0] = m_mix[i * 2 + 0] * m_gain;
            buffer[i * 2 + 1] = m_mix[i * 2 + 1] * m_gain;
            m_peak = std::max({ m_peak, std::abs(buffer[i * 2 + 0]), std::abs(buffer[i * 2 + 1]) });
        }

        // Report the signal level now and then; useful to tell silence from a broken mix without listening.
        if (++m_framesSubmitted % (AUDIO_SAMPLE_RATE / AUDIO_FRAME_SAMPLES * 10) == 0)
        {
            LOGF_INFO("audio: {} frames, peak level {:.3f} over the last 10 seconds", m_framesSubmitted, m_peak);
            m_peak = 0.0f;
        }

        XAUDIO2_BUFFER submission{};
        submission.AudioBytes = sizeof(buffer);
        submission.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
        m_source->SubmitSourceBuffer(&submission);
        return true;
    }

    // IXAudio2VoiceCallback
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(m_bufferDone); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

private:
    IXAudio2* m_xaudio{};
    IXAudio2MasteringVoice* m_master{};
    IXAudio2SourceVoice* m_source{};
    HANDLE m_bufferDone{};
    std::array<std::array<float, AUDIO_FRAME_SAMPLES * OUTPUT_CHANNELS>, BUFFER_RING_SIZE> m_buffers{};
    std::array<float, AUDIO_FRAME_SAMPLES * OUTPUT_CHANNELS> m_mix{};
    std::mutex m_mixMutex;
    bool m_mixUsed{};
    uint32_t m_nextBuffer{};
    uint64_t m_framesSubmitted{};
    float m_peak{};
    float m_gain{ 1.0f };
};

static AudioOutput g_output;
static std::atomic<bool> g_outputAvailable;

static void CallClients()
{
    for (AudioClient& client : g_clients)
    {
        if (uint32_t callback = client.callback)
            GuestToHostFunction<void>(callback, client.context.load());
    }
}

static void RunAudioDriver()
{
    g_outputAvailable = g_output.Initialize();
    if (g_outputAvailable)
    {
        LOGF_INFO("audio output: XAudio2, {} Hz stereo", AUDIO_SAMPLE_RATE);
        while (true)
        {
            if (g_output.QueuedFrames() < MAX_QUEUED_FRAMES)
            {
                CallClients();
                if (!g_output.Send())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // No client yet, or one that answers later.
            }
            else
                g_output.WaitForBuffer();
        }
    }

    // Without an audio device, keep the title's mixer running at the hardware rate and drop the frames.
    LOGF_WARNING("no audio output device, audio is disabled");
    auto next = std::chrono::steady_clock::now();
    while (true)
    {
        next += std::chrono::microseconds(1000000 * AUDIO_FRAME_SAMPLES / AUDIO_SAMPLE_RATE);
        std::this_thread::sleep_until(next);
        CallClients();
    }
}

namespace imports
{
    // The client is a pointer to { callback, context }.
    uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* client, be<uint32_t>* driver)
    {
        uint32_t index = 0;
        while (index < MAX_CLIENTS && g_clients[index].callback != 0)
            index++;
        if (index == MAX_CLIENTS)
        {
            LOGF_WARNING("audio: no room for another client");
            return X_STATUS_UNSUCCESSFUL;
        }
        uint32_t context = g_runtimeHeap.AllocGuest(4);
        *GuestPtr<be<uint32_t>>(context) = client[1].get();
        g_clients[index].context = context;
        g_clients[index].callback = client[0];
        *driver = DRIVER_HANDLE | index;
        LOGF_INFO("audio: client {} registered", index);

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
        uint32_t index = driver & 0xFF;
        if ((driver & 0xFFFF0000) == DRIVER_HANDLE && index < MAX_CLIENTS)
        {
            g_clients[index].callback = 0;
            LOGF_INFO("audio: client {} unregistered", index);
        }
        return X_STATUS_SUCCESS;
    }

    uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, be<float>* samples)
    {
        if (g_outputAvailable && samples != nullptr)
            g_output.Add(samples);

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
        static std::atomic<bool> warned;
        if (!warned.exchange(true))
            LOGF_WARNING("XMA decoding is not implemented, sounds using it will be silent");

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
