#include <stdafx.h>
#include <kernel/memory.h>
#include "command_processor.h"
#include <unordered_set>

// Guest return addresses up the stack: the callee saves its return address 8 bytes below its caller's stack pointer,
// and each frame starts with the back chain to the previous one.
static uint32_t GuestCallChain(PPCContext& ctx, uint8_t* base, uint32_t* out, uint32_t capacity)
{
    uint32_t count = 0;
    out[count++] = uint32_t(ctx.lr);
    uint32_t sp = ctx.r1.u32;
    while (count < capacity)
    {
        uint32_t previous = PPC_LOAD_U32(sp);
        if (previous <= sp || previous - sp > 0x100000)
            break;
        out[count++] = PPC_LOAD_U32(previous - 8);
        sp = previous;
    }
    return count;
}

// The engine's model draws. Diagnostics (AR_LOG_MODEL_DRAWS=<first frame>,<frames>): each call with the object (r3)
// and the guest call chain, for the CPU side frames given.
static thread_local uint32_t t_modelObject;

static void LogModelDraw(const char* name, PPCContext& ctx, uint8_t* base)
{
    if (name[4] == '4') // 826D4A70, 826D4B50: the model, for the draw markers.
        t_modelObject = ctx.r3.u32;

    // AR_HASH_MODEL_DRAWS: one line per frame and function with the call count and an order independent hash of the
    // calls (object, part, caller), cheap enough to keep the game at full speed.
    static const bool hashDraws = AR_ENV("AR_HASH_MODEL_DRAWS") != nullptr;
    if (hashDraws)
    {
        struct Totals
        {
            uint32_t frame = 0, calls = 0;
            uint64_t hash = 0;
        };
        static std::mutex mutex;
        static std::map<std::string, Totals> totals;
        std::lock_guard lock(mutex);
        auto& entry = totals[name];
        uint32_t frame = gpu::SubmittedSwaps();
        if (frame != entry.frame)
        {
            if (entry.calls != 0)
                LOGF_INFO("MH {} {} {} {:016X}", entry.frame, name, entry.calls, entry.hash);
            entry = { frame, 0, 0 };
        }
        uint64_t key = (uint64_t(ctx.r3.u32) << 32 | ctx.r4.u32) * 0x9E3779B97F4A7C15ull ^ uint64_t(uint32_t(ctx.lr)) * 0xC2B2AE3D27D4EB4Full;
        key ^= key >> 31;
        entry.hash += key * 0x100000001B3ull;
        entry.calls++;
    }

    static int64_t first = -1, frames = 0;
    static const bool enabled = []
        {
            const char* text = AR_ENV("AR_LOG_MODEL_DRAWS");
            if (!text)
                return false;
            first = atoll(text);
            const char* comma = strchr(text, ',');
            frames = comma ? atoll(comma + 1) : 1;
            return true;
        }();
    if (!enabled)
        return;
    int64_t frame = gpu::SubmittedSwaps();
    if (frame < first || frame >= first + frames)
        return;
    uint32_t chain[12];
    uint32_t count = GuestCallChain(ctx, base, chain, 12);
    std::string text;
    for (uint32_t i = 0; i < count; i++)
        text += fmt::format(" {:08X}", chain[i]);
    LOGF_INFO("M {} {} {:08X} {:08X} {:08X}:{}", frame, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, text);
}

#define AR_MODEL_DRAW_HOOK(address) \
    PPC_FUNC_IMPL(__imp__sub_##address); \
    PPC_FUNC(sub_##address) \
    { \
        LogModelDraw(#address, ctx, base); \
        __imp__sub_##address(ctx, base); \
    }

AR_MODEL_DRAW_HOOK(826D4A70)
AR_MODEL_DRAW_HOOK(826D4B50)
AR_MODEL_DRAW_HOOK(826CED10)
AR_MODEL_DRAW_HOOK(826FC890)
AR_MODEL_DRAW_HOOK(826FC960)
AR_MODEL_DRAW_HOOK(826FBD10)

// Diagnostics (AR_LOG_DRAW_CALLS): the title's D3D functions that write draw packets, with calls per second, to find
// the title's draw entry points.
namespace
{
    void CountDrawCall(const char* name)
    {
        static const bool enabled = AR_ENV("AR_LOG_DRAW_CALLS") != nullptr;
        if (!enabled)
            return;
        static std::mutex mutex;
        static std::map<std::string, uint32_t> counts;
        static auto lastReport = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        counts[name]++;
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport < std::chrono::seconds(5))
            return;
        lastReport = now;
        std::string text;
        for (const auto& [function, count] : counts)
            text += fmt::format(" {} {};", function, count);
        LOGF_INFO("draw packet writers in 5 s:{}", text);
        counts.clear();
    }
}

#define AR_COUNT_DRAW_HOOK(address) \
    PPC_FUNC_IMPL(__imp__sub_##address); \
    PPC_FUNC(sub_##address) \
    { \
        CountDrawCall(#address); \
        __imp__sub_##address(ctx, base); \
    }

// The engine's render queue execution (r3 = queue): two item arrays at +76/+80, chosen by the counter at +36
// ((counter - 1) & 1), and the item count at +72; each item is an object whose second virtual function draws it.
//
// Diagnostics (AR_LOG_QUEUES): every execution with the queue, counter, count and the guest caller.
PPC_FUNC_IMPL(__imp__sub_826F4720);
PPC_FUNC(sub_826F4720)
{
    static const bool logQueues = AR_ENV("AR_LOG_QUEUES") != nullptr;
    if (logQueues)
    {
        uint32_t queue = ctx.r3.u32;
        uint32_t counter = PPC_LOAD_U32(queue + 36);
        uint32_t array = PPC_LOAD_U32(queue + ((((counter - 1) & 1) + 19) * 4));
        LOGF_INFO("Q {:08X} counter {} count {} array {:08X} other {:08X} caller {:08X} thread {}", queue, counter,
            PPC_LOAD_U32(queue + 72), array, PPC_LOAD_U32(queue + (((counter & 1) + 19) * 4)), uint32_t(ctx.lr),
            GetCurrentThreadId());

        // AR_LOG_QUEUE_ITEMS=<queue>: that queue's items (object and its draw function).
        static const std::string itemsOf = AR_ENV("AR_LOG_QUEUE_ITEMS") ? AR_ENV("AR_LOG_QUEUE_ITEMS") : "";
        if (!itemsOf.empty() && itemsOf.find(fmt::format("{:08X}", queue)) != std::string::npos)
        {
            std::string text = fmt::format("{} {:08X}", gpu::SubmittedSwaps(), queue);
            uint32_t count = PPC_LOAD_U32(queue + 72);
            for (uint32_t i = 0; i < count && i < 512; i++)
            {
                uint32_t object = PPC_LOAD_U32(array + i * 4);
                text += fmt::format(" {:08X}/{:08X}", object, PPC_LOAD_U32(PPC_LOAD_U32(object) + 4));
            }
            LOGF_INFO("QI {}", text);
        }
    }
    __imp__sub_826F4720(ctx, base);
}

AR_COUNT_DRAW_HOOK(822351E8)
AR_COUNT_DRAW_HOOK(82235340)
AR_COUNT_DRAW_HOOK(8224A558)
AR_COUNT_DRAW_HOOK(8224BB50)
// Diagnostics (AR_MARK_DRAWS): a NOP packet before each draw the title writes, holding a hash of the guest call chain
// and the model being drawn, so draws seen by the command processor (the shadow log) can be traced to the code that
// issued them. Each new chain is logged once (MK lines).
static void MarkDraw(PPCContext& ctx, uint8_t* base)
{
    static const bool enabled = AR_ENV("AR_MARK_DRAWS") != nullptr;
    if (!enabled)
        return;
    // r3 is the device; its command buffer write pointer (+48) is below the limit (+56).
    uint32_t device = ctx.r3.u32;
    uint32_t write = PPC_LOAD_U32(device + 48), limit = PPC_LOAD_U32(device + 56);
    static bool once = [&] { LOGF_INFO("draw markers: device {:08X} write {:08X} limit {:08X}", device, write, limit); return true; }();
    if (write > limit || limit - write > 0x100000)
        return;

    uint32_t chain[12];
    uint32_t count = GuestCallChain(ctx, base, chain, 12);
    uint32_t hash = 0x811C9DC5;
    for (uint32_t i = 0; i < count; i++)
        hash = (hash ^ chain[i]) * 0x01000193;
    {
        static std::mutex mutex;
        static std::unordered_set<uint32_t> logged;
        std::lock_guard lock(mutex);
        if (logged.insert(hash).second)
        {
            std::string text;
            for (uint32_t i = 0; i < count; i++)
                text += fmt::format(" {:08X}", chain[i]);
            LOGF_INFO("MK {:08X}:{}", hash, text);
        }
    }

    // The write pointer (device+48) points at the last word written; the limit (+56) leaves room for a draw beyond it.
    if (write + 16 > limit)
        return;
    PPC_STORE_U32(write + 4, 0xC0021000); // Type 3 NOP, 3 words.
    PPC_STORE_U32(write + 8, 0x41524D4B); // 'ARMK'
    PPC_STORE_U32(write + 12, hash);
    PPC_STORE_U32(write + 16, t_modelObject);
    PPC_STORE_U32(device + 48, write + 16);
}

#define AR_MARK_DRAW_HOOK(address)     PPC_FUNC_IMPL(__imp__sub_##address);     PPC_FUNC(sub_##address)     {         CountDrawCall(#address);         MarkDraw(ctx, base);         __imp__sub_##address(ctx, base);     }

AR_MARK_DRAW_HOOK(82250778)
AR_MARK_DRAW_HOOK(82252208)
AR_MARK_DRAW_HOOK(82251DF0)

// The UI's sprite draw (a render queue item's draw function, r3 = item). Diagnostics (AR_LOG_UI_ITEMS=<frame>,<count>):
// the item's first 48 words in those frames.
PPC_FUNC_IMPL(__imp__sub_8280FA08);
PPC_FUNC(sub_8280FA08)
{
    static int64_t first = -1, frames = 0;
    static const bool enabled = []
        {
            const char* text = AR_ENV("AR_LOG_UI_ITEMS");
            if (!text)
                return false;
            first = atoll(text);
            const char* comma = strchr(text, ',');
            frames = comma ? atoll(comma + 1) : 1;
            return true;
        }();
    int64_t frame = gpu::SubmittedSwaps();
    if (enabled && frame >= first && frame < first + frames)
    {
        std::string text;
        for (uint32_t i = 0; i < 48; i++)
            text += fmt::format(" {:08X}", PPC_LOAD_U32(ctx.r3.u32 + i * 4));
        LOGF_INFO("UI {} {:08X}:{}", frame, ctx.r3.u32, text);
    }
    __imp__sub_8280FA08(ctx, base);
}

void WatchGuestWords(const std::vector<uint32_t>& addresses);

// A UI element's per-update effects (r3 = element; its time in frames is the float at +116). Diagnostics
// (AR_WATCH_UI_TIME=<seconds>): after that long, logs who writes the time of the next elements seen.
PPC_FUNC_IMPL(__imp__sub_827A07B0);
PPC_FUNC(sub_827A07B0)
{
    // AR_LOG_UI_TIME: every 5 s, how far one UI element's time advanced per real second (1 is normal speed).
    static const bool logRate = AR_ENV("AR_LOG_UI_TIME") != nullptr;
    if (logRate)
    {
        static uint32_t element;
        static float lastTime;
        static auto lastReport = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        uint32_t bits = PPC_LOAD_U32(ctx.r3.u32 + 116);
        float time;
        memcpy(&time, &bits, 4);
        if (element == 0 || element == ctx.r3.u32)
        {
            double seconds = std::chrono::duration<double>(now - lastReport).count();
            if (element == 0)
            {
                element = ctx.r3.u32;
                lastTime = time;
                lastReport = now;
            }
            else if (seconds >= 5.0)
            {
                LOGF_INFO("UI time rate {:.2f} (element {:08X}, {:.1f} s of UI time in {:.1f} s)", (time - lastTime) / seconds,
                    element, time - lastTime, seconds);
                lastTime = time;
                lastReport = now;
            }
        }
    }

    static const char* text = AR_ENV("AR_WATCH_UI_TIME");
    if (text)
    {
        static const auto launched = std::chrono::steady_clock::now();
        static std::vector<uint32_t> elements;
        static bool armed;
        if (!armed && std::chrono::steady_clock::now() - launched > std::chrono::seconds(atoi(text)))
        {
            uint32_t time = ctx.r3.u32 + 116;
            if (std::find(elements.begin(), elements.end(), time) == elements.end())
            {
                elements.push_back(time);
                uint32_t bits = PPC_LOAD_U32(time);
                float value;
                memcpy(&value, &bits, 4);
                LOGF_INFO("UI element {:08X} time {}", ctx.r3.u32, value);
            }
            if (elements.size() >= 6)
            {
                armed = true;
                WatchGuestWords(elements);
            }
        }
    }
    __imp__sub_827A07B0(ctx, base);
}
