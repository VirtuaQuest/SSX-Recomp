#include <stdafx.h>
#include <kernel/memory.h>
#include <gpu/video_settings.h>

// The title's QueryPerformanceCounter: stores the 64-bit time base at r3 and returns 1.
//
// Diagnostics (AR_LOG_QPC): calls per guest return address, every 5 seconds.
PPC_FUNC_IMPL(__imp__sub_8245B458);
PPC_FUNC(sub_8245B458)
{
    static const bool logCallers = AR_ENV("AR_LOG_QPC") != nullptr;
    if (logCallers)
    {
        static std::mutex mutex;
        static std::map<uint32_t, uint32_t> counts;
        static auto lastReport = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        counts[uint32_t(ctx.lr)]++;
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(5))
        {
            lastReport = now;
            std::string text;
            for (auto [caller, count] : counts)
                text += fmt::format(" {:08X} {};", caller, count);
            LOGF_INFO("QueryPerformanceCounter callers in 5 s:{}", text);
            counts.clear();
        }
    }

    __imp__sub_8245B458(ctx, base);
}

// The title's per-frame timing update: measures the time since the last frame and sets the game's time step (in 60 Hz
// frames) at 0x830778C8, from constants at 0x820A4130.
//
// Diagnostics (AR_LOG_TIMESTEP): the constants once and the time step every 30 calls.
PPC_FUNC_IMPL(__imp__sub_82BD7EC8);
PPC_FUNC(sub_82BD7EC8)
{
    // The title takes the measured step only between 1.01 and 2 (falling back to 1 below): frames shorter than
    // 30 Hz still advance the game by a whole 30 Hz frame, so it runs fast above 30 fps. Above 30 fps, the minimum is
    // lowered so the step follows the frame time (AR_NO_TIMESTEP_FIX keeps the original).
    static const bool fixStep = gpu::g_videoSettings.frameRate > 30 && !AR_ENV("AR_NO_TIMESTEP_FIX");
    if (fixStep)
    {
        static const float minimum = 0.2f;
        uint32_t bits;
        memcpy(&bits, &minimum, 4);
        PPC_STORE_U32(0x820A4130 + 2176, bits);
    }

    __imp__sub_82BD7EC8(ctx, base);

    static const bool logStep = AR_ENV("AR_LOG_TIMESTEP") != nullptr;
    if (!logStep)
        return;

    auto readFloat = [&](uint32_t address)
        {
            uint32_t bits = PPC_LOAD_U32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        };
    static bool loggedConstants;
    if (!loggedConstants)
    {
        loggedConstants = true;
        LOGF_INFO("time step constants: default {} max {} scale {} min {} (+212 {} +124 {} +644 {} +1104 {})", readFloat(0x820A4130 + 16),
            readFloat(0x820A4130 + 68), readFloat(0x820A4130 + 716), readFloat(0x820A4130 + 2176), readFloat(0x820A4130 + 212),
            readFloat(0x820A4130 + 124), readFloat(0x820A4130 + 644), readFloat(0x820A4130 + 1104));
    }
    static uint32_t calls;
    if (++calls % 30 == 0)
        LOGF_INFO("time step {} (measured {})", readFloat(0x830778C8), readFloat(0x830C6B7C));
}
