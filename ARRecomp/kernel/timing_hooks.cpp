#include <stdafx.h>
#include <kernel/memory.h>
#include <gpu/video_settings.h>

// D3D's block on fence (device r3, fence r4): kicks the command buffer when the fence is the newest one, then spins
// until the GPU's completed fence (*(device + 11024)) reaches it (the device's newest fence is at device + 11036).
//
// Diagnostics (AR_LOG_FENCE_WAITS): time spent waiting per caller and how far behind the newest fence the target was.
PPC_FUNC_IMPL(__imp__sub_82238EC8);
PPC_FUNC(sub_82238EC8)
{
    static const bool logWaits = AR_ENV("AR_LOG_FENCE_WAITS") != nullptr;
    if (!logWaits)
    {
        __imp__sub_82238EC8(ctx, base);
        return;
    }

    uint32_t caller = uint32_t(ctx.lr), device = ctx.r3.u32, fence = ctx.r4.u32;
    uint32_t behind = PPC_LOAD_U32(device + 11036) - fence;
    auto start = std::chrono::steady_clock::now();
    __imp__sub_82238EC8(ctx, base);
    double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    struct Entry
    {
        uint32_t calls = 0;
        double total = 0.0;
        std::map<uint32_t, uint32_t> behind;
    };
    static std::mutex mutex;
    static std::map<uint32_t, Entry> entries;
    static auto lastReport = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    auto& entry = entries[caller];
    entry.calls++;
    entry.total += waited;
    entry.behind[std::min(behind, 9u)]++;
    auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(5))
        return;
    lastReport = now;
    std::string text;
    for (const auto& [address, value] : entries)
    {
        std::string distances;
        for (auto [distance, count] : value.behind)
            distances += fmt::format(" {}:{}", distance, count);
        text += fmt::format("\n    caller {:08X}: {} waits, {:.0f} ms (fences behind newest:{})", address, value.calls, value.total, distances);
    }
    LOGF_INFO("D3D fence waits in 5 s:{}", text);
    entries.clear();
}

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

void WatchGuestWords(const std::vector<uint32_t>& addresses);

// Diagnostics (AR_SCAN_TIMERS=<update number>|@<seconds>): finds timers that count frames instead of time. From that update on,
// guest memory (the virtual heap, the image's data and the physical heap) is compared over 5 updates; words that
// change by exactly 1 every update (integers, or floats by 1.0) are logged as TIMER lines. Run it at 120 fps: timers
// that follow the game's time step then change by 0.25 (30 Hz units) or 0.5 (60 Hz units) per update, so what is
// left counts updates and runs fast at high frame rates.
static void ScanTimers(uint8_t* base)
{
    // "@<seconds>" starts at the first update after that many seconds instead.
    static const char* text = AR_ENV("AR_SCAN_TIMERS");
    if (!text)
        return;
    static const auto launched = std::chrono::steady_clock::now();
    static int64_t start = text[0] == '@' ? -1 : atoll(text);
    static int64_t update;
    if (start < 0)
    {
        if (std::chrono::steady_clock::now() - launched < std::chrono::seconds(atoi(text + 1)))
        {
            update++;
            return;
        }
        start = update;
    }
    int64_t index = update++ - start;
    if (index < 0 || index > 5)
        return;

    static constexpr std::pair<uint32_t, uint32_t> ranges[] = { { 0x40000000, 0x50000000 }, { 0x83000000, 0x83B00000 },
        { 0xA0000000, 0xC0000000 } };
    static std::vector<uint32_t> snapshots[std::size(ranges)];
    // Candidates: address and kind (0 integer up, 1 integer down, 2 float up, 3 float down), alive while consistent.
    static std::vector<std::pair<uint32_t, uint8_t>> candidates;
    auto kindOf = [](uint32_t before, uint32_t after) -> int
        {
            if (after == before + 1 && after < 0x01000000)
                return 0;
            if (after + 1 == before && before < 0x01000000)
                return 1;
            float a, b;
            memcpy(&a, &before, 4);
            memcpy(&b, &after, 4);
            if (std::isfinite(a) && std::isfinite(b) && std::abs(b) < 1e6f && std::abs(a) > 1e-3f)
            {
                if (b - a == 1.0f)
                    return 2;
                if (a - b == 1.0f)
                    return 3;
            }
            return -1;
        };

    auto started = std::chrono::steady_clock::now();
    if (index == 0)
        candidates.clear();
    for (size_t r = 0; r < std::size(ranges); r++)
    {
        auto [first, end] = ranges[r];
        uint32_t words = (end - first) / 4;
        auto& snapshot = snapshots[r];
        const auto* memory = reinterpret_cast<const uint32_t*>(base + first);
        if (index == 0)
        {
            snapshot.resize(words);
            for (uint32_t i = 0; i < words; i++)
                snapshot[i] = ByteSwap(memory[i]);
            continue;
        }
        if (index == 1)
        {
            for (uint32_t i = 0; i < words; i++)
            {
                uint32_t value = ByteSwap(memory[i]);
                if (value != snapshot[i])
                {
                    int kind = kindOf(snapshot[i], value);
                    if (kind >= 0)
                        candidates.push_back({ first + i * 4, uint8_t(kind) });
                }
                snapshot[i] = value;
            }
        }
    }
    if (index >= 2)
    {
        std::vector<std::pair<uint32_t, uint8_t>> alive;
        for (auto [address, kind] : candidates)
        {
            size_t r = 0;
            while (address >= ranges[r].second)
                r++;
            uint32_t& before = snapshots[r][(address - ranges[r].first) / 4];
            uint32_t value = PPC_LOAD_U32(address);
            if (kindOf(before, value) == kind)
                alive.push_back({ address, kind });
            before = value;
        }
        candidates = std::move(alive);
    }
    LOGF_INFO("timer scan {}: {} candidates ({:.0f} ms)", index, candidates.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    if (index == 5)
    {
        static const char* kinds[] = { "int+1", "int-1", "float+1", "float-1" };
        for (auto [address, kind] : candidates)
        {
            uint32_t value = PPC_LOAD_U32(address);
            float asFloat;
            memcpy(&asFloat, &value, 4);
            LOGF_INFO("TIMER {:08X} {} {:08X} {}", address, kinds[kind], value, asFloat);
        }

        // AR_SCAN_TIMERS_WATCH: then logs who writes a sample of them (two per group of similar values).
        if (AR_ENV("AR_SCAN_TIMERS_WATCH"))
        {
            std::map<std::pair<uint8_t, uint32_t>, int> groups;
            std::vector<uint32_t> watched;
            for (auto [address, kind] : candidates)
            {
                uint32_t value = PPC_LOAD_U32(address);
                if (groups[{ kind, value >> 6 }]++ < 2 && watched.size() < 40)
                    watched.push_back(address);
            }
            LOGF_INFO("watching {} timers", watched.size());
            WatchGuestWords(watched);
        }
    }
}

// The UI's updates hand their elements (HUD, radar, dialogue boxes, result screens) a fixed 1/30 s per update, so
// above 30 fps the UI ran 2-4 times too fast. Mid-function hooks after each loads that step into f31 (see AR.toml):
// scaled by the game's time step (30 Hz frames, 0x830778C8), which follows the frame time above 30 fps.
// AR_NO_UI_TIMESTEP_FIX keeps the fixed step; AR_LOG_UI_TIMESTEP logs how often each hook runs (every 5 s).
static void ScaleUiStep(PPCRegister& f31, int site)
{
    static const bool logCalls = AR_ENV("AR_LOG_UI_TIMESTEP") != nullptr;
    if (logCalls)
    {
        static std::mutex mutex;
        static uint32_t calls[7];
        static auto lastReport = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        calls[site]++;
        if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(5))
        {
            lastReport = std::chrono::steady_clock::now();
            LOGF_INFO("UI step hooks in 5 s: {} {} {} {} {} {}", calls[1], calls[2], calls[3], calls[4], calls[5], calls[6]);
            memset(calls, 0, sizeof(calls));
        }
    }

    static const bool fix = gpu::g_videoSettings.frameRate > 30 && !AR_ENV("AR_NO_UI_TIMESTEP_FIX");
    if (!fix)
        return;
    uint32_t bits = *reinterpret_cast<const be<uint32_t>*>(g_memory.Translate(0x830778C8));
    float step;
    memcpy(&step, &bits, 4);
    if (std::isfinite(step) && step > 0.0f && step <= 4.0f)
        f31.f64 = double(float(f31.f64 * step));
}

void UiTimeStepHook(PPCRegister& f31) { ScaleUiStep(f31, 1); } // sub_827BA3B0, the UI manager.
void UiTimeStepHook2(PPCRegister& f31) { ScaleUiStep(f31, 2); } // sub_82789708, its twin.
void UiTimeStepHook3(PPCRegister& f31) { ScaleUiStep(f31, 3); } // sub_827B5BC8
void UiTimeStepHook4(PPCRegister& f31) { ScaleUiStep(f31, 4); } // sub_827B6690
void UiTimeStepHook5(PPCRegister& f31) { ScaleUiStep(f31, 5); } // sub_827B6E40
void UiTimeStepHook6(PPCRegister& f31) { ScaleUiStep(f31, 6); } // sub_827F7828

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
    ScanTimers(base);

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
