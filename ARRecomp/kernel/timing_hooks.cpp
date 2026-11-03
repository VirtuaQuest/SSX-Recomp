#include <stdafx.h>
#include <kernel/memory.h>
#include <gpu/video_settings.h>
#include <gpu/d3d12_renderer.h>

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
void StopWatchingGuestWords();

// Whether a watch started here is active: starting another then would lose track of the pages it protects.
static bool g_watchActive;
static int g_huntWatchLeft;

// Diagnostics (AR_SCAN_TIMERS=<update number>|@<seconds>): finds timers that count frames instead of time. From that update on,
// guest memory (the virtual heap, the image's data and the physical heap) is compared over 5 updates; integers changing
// by 1 and floats changing by the same amount every update are logged as TIMER lines. Run it at 120 fps: timers
// that follow the game's time step then change by 0.25 (30 Hz units) or 0.5 (60 Hz units) per update, so what is
// left counts updates and runs fast at high frame rates.
static std::atomic<int> g_timerScanRequest; // 1 scan, 2 scan and watch (remote control, AR_REMOTE).
static bool g_watchTimers;

void RequestTimerScan(bool watch)
{
    g_timerScanRequest = watch ? 2 : 1;
}

static void ScanTimers(uint8_t* base)
{
    // "@<seconds>" starts at the first update after that many seconds instead.
    static const char* text = AR_ENV("AR_SCAN_TIMERS");
    static const auto launched = std::chrono::steady_clock::now();
    static int64_t start = !text ? INT64_MAX : text[0] == '@' ? -1 : atoll(text);
    static int64_t update;
    static const bool watchFromEnvironment = AR_ENV("AR_SCAN_TIMERS_WATCH") != nullptr;
    // AR_SCAN_KEY: F9 starts a scan while playing, F10 one that also watches what it finds for a moment.
    static const bool scanKey = AR_ENV("AR_SCAN_KEY") != nullptr;
    if (scanKey)
    {
        // The low bit also reports presses from before this run: only act on a key going down.
        static bool f9Down, f10Down;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f9 && !f9Down)
            RequestTimerScan(false);
        if (f10 && !f10Down)
            RequestTimerScan(true);
        f9Down = f9;
        f10Down = f10;
    }
    if (int request = g_timerScanRequest.exchange(0))
    {
        if (!g_watchActive)
        {
            start = update;
            g_watchTimers = request == 2;
        }
    }
    else if (text && start == update)
        g_watchTimers = watchFromEnvironment;
    if (start == INT64_MAX)
        return;
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
    // A watch is only kept for a moment: its page faults slow the game to a few frames per second.
    if (index == 15 && g_watchTimers)
    {
        StopWatchingGuestWords();
        g_watchActive = false;
        LOGF_INFO("stopped watching timers");
    }
    if (index < 0 || index > 5)
        return;
    if (index == 0)
        LOGF_INFO("timer scan at frame {}", gpu::d3d12::GetFrameNumber());

    static constexpr std::pair<uint32_t, uint32_t> ranges[] = { { 0x40000000, 0x50000000 }, { 0x83000000, 0x83B00000 },
        { 0xA0000000, 0xC0000000 } };
    static std::vector<uint32_t> snapshots[std::size(ranges)];
    // Candidates, alive while consistent: integers changing by 1, or floats changing by the same amount every update
    // (those following the time step don't: the scan's own slow first updates change it).
    struct Candidate
    {
        uint32_t address;
        uint8_t kind; // 0 integer up, 1 integer down, 2 float.
        float delta;
    };
    static std::vector<Candidate> candidates;
    auto classify = [](uint32_t before, uint32_t after, float& delta) -> int
        {
            if (after == before + 1 && after < 0x01000000)
                return 0;
            if (after + 1 == before && before < 0x01000000)
                return 1;
            float a, b;
            memcpy(&a, &before, 4);
            memcpy(&b, &after, 4);
            if (!std::isfinite(a) || !std::isfinite(b) || std::abs(b) >= 1e6f || std::abs(a) <= 1e-3f || a == b)
                return -1;
            delta = b - a;
            return 2;
        };
    auto stillAlive = [&](const Candidate& candidate, uint32_t before, uint32_t after)
        {
            float delta = 0.0f;
            int kind = classify(before, after, delta);
            if (kind != candidate.kind)
                return false;
            if (kind != 2)
                return true;
            float magnitude;
            memcpy(&magnitude, &after, 4);
            return std::abs(delta - candidate.delta) <= std::abs(candidate.delta) * 1e-3f + std::abs(magnitude) * 2.4e-7f;
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
                    float delta = 0.0f;
                    int kind = classify(snapshot[i], value, delta);
                    if (kind >= 0)
                        candidates.push_back({ first + i * 4, uint8_t(kind), delta });
                }
                snapshot[i] = value;
            }
        }
    }
    if (index >= 2)
    {
        std::vector<Candidate> alive;
        for (const auto& candidate : candidates)
        {
            size_t r = 0;
            while (candidate.address >= ranges[r].second)
                r++;
            uint32_t& before = snapshots[r][(candidate.address - ranges[r].first) / 4];
            uint32_t value = PPC_LOAD_U32(candidate.address);
            if (stillAlive(candidate, before, value))
                alive.push_back(candidate);
            before = value;
        }
        candidates = std::move(alive);
    }
    LOGF_INFO("timer scan {}: {} candidates ({:.0f} ms)", index, candidates.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    if (index == 5)
    {
        static const char* kinds[] = { "int+1", "int-1", "float" };
        for (const auto& candidate : candidates)
        {
            uint32_t value = PPC_LOAD_U32(candidate.address);
            float asFloat;
            memcpy(&asFloat, &value, 4);
            LOGF_INFO("TIMER {:08X} {} {:08X} {} delta {}", candidate.address, kinds[candidate.kind], value, asFloat, candidate.delta);
        }

        // Watching: logs who writes a sample of them, for 10 updates. Values really moving by a fixed step if there are any
        // (one per memory page), otherwise two per group of similar values.
        if (g_watchTimers)
        {
            std::vector<uint32_t> watched;
            std::set<uint32_t> pages;
            for (const auto& candidate : candidates)
            {
                if (candidate.kind == 2 && std::abs(candidate.delta) >= 0.005f &&
                    pages.insert(candidate.address >> 12).second && watched.size() < 32)
                    watched.push_back(candidate.address);
            }
            std::map<std::pair<uint8_t, uint32_t>, int> groups;
            for (const auto& candidate : candidates)
            {
                if (!pages.empty())
                    break;
                uint32_t value = PPC_LOAD_U32(candidate.address);
                if (groups[{ candidate.kind, value >> 6 }]++ < 2 && watched.size() < 40)
                    watched.push_back(candidate.address);
            }
            g_watchActive = true;
            LOGF_INFO("watching {} timers", watched.size());
            WatchGuestWords(watched);
        }
    }
}

// Diagnostics for remote control (AR_REMOTE): a search for values in guest memory, like a cheat finder, and a tracker.
// "find new" snapshots the heaps; "find changed" / "find same" keep the floats that changed / stayed since the last
// step (the first step after "new" keeps only floats that changed); "find list" logs up to 400 survivors as FIND
// lines. "track <address> [floats]" logs that many floats (3) there every 6 updates as TRACK lines ("track 0" stops).
static std::mutex g_findMutex;
static std::deque<std::string> g_findRequests;

void RequestFind(const std::string& request)
{
    std::lock_guard lock(g_findMutex);
    g_findRequests.push_back(request);
}

static void RunFind(uint8_t* base)
{
    static constexpr std::pair<uint32_t, uint32_t> ranges[] = { { 0x40000000, 0x50000000 }, { 0x82000000, 0x83B00000 },
        { 0xA0000000, 0xC0000000 } };
    static std::vector<uint32_t> snapshots[std::size(ranges)];
    static std::vector<std::pair<uint32_t, uint32_t>> candidates; // Address and value at the last step.
    static bool fresh;
    static uint32_t trackAddress, trackCount;
    static uint64_t update;
    update++;
    if (g_huntWatchLeft > 0 && --g_huntWatchLeft == 0)
    {
        StopWatchingGuestWords();
        g_watchActive = false;
        LOGF_INFO("stopped watching");
    }
    if (trackAddress && update % 6 == 0)
    {
        std::string text;
        for (uint32_t i = 0; i < trackCount; i++)
        {
            uint32_t bits = PPC_LOAD_U32(trackAddress + i * 4);
            float value;
            memcpy(&value, &bits, 4);
            text += fmt::format(" {:.3f}", value);
        }
        LOGF_INFO("TRACK {} {:08X}{}", gpu::d3d12::GetFrameNumber(), trackAddress, text);
    }

    std::string request;
    {
        std::lock_guard lock(g_findMutex);
        if (g_findRequests.empty())
            return;
        request = g_findRequests.front();
        g_findRequests.pop_front();
    }
    auto isFloat = [](uint32_t bits)
        {
            float value;
            memcpy(&value, &bits, 4);
            return std::isfinite(value) && std::abs(value) > 1e-3f && std::abs(value) < 1e6f;
        };
    if (request.rfind("watch", 0) == 0)
    {
        // "find watch <addresses>": logs who writes these guest words, for 30 updates.
        std::vector<uint32_t> watched;
        const char* text = request.c_str() + 5;
        char* end;
        for (unsigned long address = strtoul(text, &end, 16); end != text; address = strtoul(text, &end, 16))
        {
            watched.push_back(uint32_t(address));
            text = end;
        }
        if (!watched.empty() && !g_watchActive)
        {
            g_watchActive = true;
            g_huntWatchLeft = 30;
            WatchGuestWords(watched);
        }
        return;
    }
    if (request.rfind("track", 0) == 0)
    {
        unsigned address = 0, count = 3;
        sscanf(request.c_str() + 5, "%x %u", &address, &count);
        trackAddress = address;
        trackCount = std::min(count, 32u);
        return;
    }
    if (request == "new")
    {
        candidates.clear();
        for (size_t r = 0; r < std::size(ranges); r++)
        {
            auto [first, end] = ranges[r];
            snapshots[r].resize((end - first) / 4);
            memcpy(snapshots[r].data(), base + first, end - first);
        }
        fresh = true;
        LOGF_INFO("FIND new: snapshot taken");
        return;
    }
    bool changed = request == "changed";
    if (changed || request == "same")
    {
        if (fresh)
        {
            fresh = false;
            for (size_t r = 0; r < std::size(ranges); r++)
            {
                auto [first, end] = ranges[r];
                const auto* memory = reinterpret_cast<const uint32_t*>(base + first);
                for (uint32_t i = 0; i < snapshots[r].size(); i++)
                {
                    uint32_t value = ByteSwap(memory[i]);
                    if (memory[i] != snapshots[r][i] && isFloat(value) && isFloat(ByteSwap(snapshots[r][i])))
                        candidates.push_back({ first + i * 4, value });
                }
                snapshots[r] = {};
            }
        }
        else
        {
            std::vector<std::pair<uint32_t, uint32_t>> alive;
            for (auto [address, before] : candidates)
            {
                uint32_t value = PPC_LOAD_U32(address);
                if ((value != before) == changed && isFloat(value))
                    alive.push_back({ address, value });
            }
            candidates = std::move(alive);
        }
        LOGF_INFO("FIND {}: {} candidates", request, candidates.size());
        return;
    }
    if (request == "list")
    {
        for (size_t i = 0; i < candidates.size() && i < 400; i++)
        {
            uint32_t bits = PPC_LOAD_U32(candidates[i].first);
            float value;
            memcpy(&value, &bits, 4);
            LOGF_INFO("FIND {:08X} {}", candidates[i].first, value);
        }
    }
}

// The UI's updates hand their elements (HUD, radar, dialogue boxes, result screens) a fixed 1/30 s per update, so
// above 30 fps the UI ran 2-4 times too fast. Mid-function hooks after each load of that step into f31 (see AR.toml)
// scale it by the game's time step (30 Hz frames, 0x830778C8), which follows the frame time above 30 fps. The constant
// itself has to stay: character code multiplies it by the time step, so scaling it put characters in slow motion.
// AR_NO_UI_TIMESTEP_FIX keeps the fixed step; AR_LOG_UI_TIMESTEP logs how often each hook runs (every 5 s).
// What the fixed step is multiplied by: the game's time step above 30 fps, otherwise 1.
static float UiStepScale()
{
    static const bool fix = gpu::g_videoSettings.frameRate > 30 && !AR_ENV("AR_NO_UI_TIMESTEP_FIX");
    if (!fix)
        return 1.0f;
    uint32_t bits = *reinterpret_cast<const be<uint32_t>*>(g_memory.Translate(0x830778C8));
    float step;
    memcpy(&step, &bits, 4);
    return std::isfinite(step) && step > 0.0f && step <= 4.0f ? step : 1.0f;
}

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

    f31.f64 = double(float(f31.f64) * UiStepScale());
}

// The rest of the UI code loads the step itself (see AR.toml): the same scaling after each of those loads. The constant
// itself has to stay, as said above.
void UiConstantHook(PPCRegister& step)
{
    step.f64 = double(float(step.f64) * UiStepScale());
}

// Updates a controller's buttons (r3: current at +0, pressed +4, released +8, pressed or repeating +12; updates held at
// +44) from the new state in r4. A held button repeats after a number of updates given by two globals, so above 30 fps
// a tap could count as 2 or 3 presses in the menus. There the count advances at 30 Hz, following the time step
// (AR_NO_INPUT_REPEAT_FIX keeps it per update; AR_LOG_INPUT_REPEAT logs the repeats).
PPC_FUNC_IMPL(__imp__sub_826E91E0);
PPC_FUNC(sub_826E91E0)
{
    static const bool fix = gpu::g_videoSettings.frameRate > 30 && !AR_ENV("AR_NO_INPUT_REPEAT_FIX");
    static const bool log = AR_ENV("AR_LOG_INPUT_REPEAT") != nullptr;
    uint32_t pad = ctx.r3.u32;
    if (!fix)
    {
        uint32_t before = PPC_LOAD_U32(pad), held = ctx.r4.u32;
        __imp__sub_826E91E0(ctx, base);
        if (log && held != 0 && before == held && PPC_LOAD_U32(pad + 12) != 0)
            LOGF_INFO("REPEAT {:08X} buttons {:08X} at frame {} delay {} period {}", pad, held, gpu::d3d12::GetFrameNumber(),
                PPC_LOAD_U32(0x831AE360), PPC_LOAD_U32(0x831AE35C));
        return;
    }

    static std::map<uint32_t, float> fractions;
    uint32_t before = PPC_LOAD_U32(pad), buttons = ctx.r4.u32;
    uint32_t changed = before ^ buttons, pressed = changed & buttons;
    PPC_STORE_U32(pad + 0, buttons);
    PPC_STORE_U32(pad + 4, pressed);
    PPC_STORE_U32(pad + 8, changed & before);
    PPC_STORE_U32(pad + 12, pressed);
    float& fraction = fractions[pad];
    if (before != buttons)
    {
        PPC_STORE_U32(pad + 44, 0);
        fraction = 0.0f;
        return;
    }
    uint32_t bits = PPC_LOAD_U32(0x830778C8);
    float step;
    memcpy(&step, &bits, 4);
    fraction += std::isfinite(step) && step > 0.0f && step <= 4.0f ? step : 1.0f;
    if (fraction < 1.0f)
        return;
    fraction = std::min(fraction - 1.0f, 1.0f);

    uint32_t count = PPC_LOAD_U32(pad + 44) + 1, delay = PPC_LOAD_U32(0x831AE360), period = PPC_LOAD_U32(0x831AE35C);
    PPC_STORE_U32(pad + 44, count);
    if (count != delay)
    {
        if (count < delay + period)
            return;
        PPC_STORE_U32(pad + 44, count - period);
    }
    PPC_STORE_U32(pad + 12, pressed | buttons);
    if (log && buttons != 0)
        LOGF_INFO("REPEAT {:08X} buttons {:08X} at frame {} delay {} period {}", pad, buttons, gpu::d3d12::GetFrameNumber(), delay, period);
}

// Updates an animated object's animation (r3: its controller; the object at +3376). Animation advances by the object's
// own time step at +944, which most objects refresh every update. Those that don't (the characters of two-character
// dialogues among them) keep the 1 they were created with, so above 30 fps their lips and idle motion ran 2-4 times too
// fast. Those get the game's time step here; they are recognised by the 1, or the step they got last time
// (AR_NO_ANIM_STEP_FIX keeps the 1; AR_LOG_ANIM_STEP logs how many objects this concerns, every 5 s).
PPC_FUNC_IMPL(__imp__sub_82BAE108);
PPC_FUNC(sub_82BAE108)
{
    static const bool fix = gpu::g_videoSettings.frameRate > 30 && !AR_ENV("AR_NO_ANIM_STEP_FIX");
    static const bool log = AR_ENV("AR_LOG_ANIM_STEP") != nullptr;
    uint32_t controller = ctx.r3.u32;
    if (fix && PPC_LOAD_U32(controller) != 0)
    {
        uint32_t owner = PPC_LOAD_U32(controller + 3376);
        if (owner != 0)
        {
            static std::atomic<uint32_t> latest{ 0x3F800000 }, previous{ 0x3F800000 };
            uint32_t stepBits = PPC_LOAD_U32(0x830778C8);
            uint32_t seen = latest.load(std::memory_order_relaxed);
            if (stepBits != seen)
            {
                previous.store(seen, std::memory_order_relaxed);
                latest.store(stepBits, std::memory_order_relaxed);
            }
            uint32_t value = PPC_LOAD_U32(owner + 944);
            if (value == 0x3F800000 || value == seen || value == previous.load(std::memory_order_relaxed))
            {
                float step;
                memcpy(&step, &stepBits, 4);
                if (std::isfinite(step) && step > 0.0f && step <= 4.0f)
                    PPC_STORE_U32(owner + 944, stepBits);
                if (log)
                {
                    static std::mutex mutex;
                    static std::set<uint32_t> owners;
                    static auto lastReport = std::chrono::steady_clock::now();
                    std::lock_guard lock(mutex);
                    owners.insert(owner);
                    if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(5))
                    {
                        lastReport = std::chrono::steady_clock::now();
                        LOGF_INFO("ANIMSTEP {} objects at frame {} (step {}), vtable of one {:08X}", owners.size(), gpu::d3d12::GetFrameNumber(), step,
                            PPC_LOAD_U32(owner));
                        owners.clear();
                    }
                }
            }
        }
    }
    __imp__sub_82BAE108(ctx, base);
}

// Advances a UI element's time (at +116) and its children by f1. Nested elements (the two-character dialogues' among
// them) get the fixed step from callers like sub_827EC068, which load it themselves: scale it here when it still is
// the fixed step.
PPC_FUNC_IMPL(__imp__sub_827EAEE0);
PPC_FUNC(sub_827EAEE0)
{
    if (std::abs(float(ctx.f1.f64) - 1.0f / 30.0f) < 1e-6f)
        ctx.f1.f64 = double(float(ctx.f1.f64) * UiStepScale());
    __imp__sub_827EAEE0(ctx, base);
}

// Characters move toward a target each update, at most 1.5 units per update (0x820A4130 + 644, shared with other code).
// Ordinary movement stays below it and already follows the time step, but launches from vents and ramps hit it, so above
// 30 fps they went up 2-4 times too fast. Hooks after each load of the limit scale it by the time step
// (AR_NO_MOTION_CAP_FIX keeps it).
void MotionCapHook(PPCRegister& limit)
{
    static const bool fix = gpu::g_videoSettings.frameRate > 30 && !AR_ENV("AR_NO_MOTION_CAP_FIX");
    if (!fix)
        return;
    uint32_t bits = *reinterpret_cast<const be<uint32_t>*>(g_memory.Translate(0x830778C8));
    float step;
    memcpy(&step, &bits, 4);
    if (std::isfinite(step) && step > 0.0f && step <= 4.0f)
        limit.f64 = double(float(limit.f64) * step);
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
    RunFind(base);

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
