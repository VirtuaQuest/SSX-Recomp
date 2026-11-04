#include <stdafx.h>
#include <gpu/video_settings.h>
#include <kernel/memory.h>
#include "command_processor.h"
#include <unordered_set>
#include <kernel/keyboard_mouse.h>

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
        if (!itemsOf.empty() && (itemsOf == "all" || itemsOf.find(fmt::format("{:08X}", queue)) != std::string::npos))
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

// The world's solid geometry queue, the same shape but with pairs (object, data). AR_LOG_QUEUES logs it too.
PPC_FUNC_IMPL(__imp__sub_821C5270);
PPC_FUNC(sub_821C5270)
{
    static const bool logQueues = AR_ENV("AR_LOG_QUEUES") != nullptr;
    if (logQueues)
    {
        uint32_t queue = ctx.r3.u32, counter = PPC_LOAD_U32(queue + 36), count = PPC_LOAD_U32(queue + 72);
        uint32_t array = PPC_LOAD_U32(queue + ((((counter - 1) & 1) + 19) * 4));
        LOGF_INFO("QW {} {:08X} counter {} count {} array {:08X} other {:08X} first {:08X} {:08X} caller {:08X}", gpu::SubmittedSwaps(), queue, counter, count, array,
            PPC_LOAD_U32(queue + (((counter & 1) + 19) * 4)), count ? PPC_LOAD_U32(array) : 0, count ? PPC_LOAD_U32(array + 4) : 0, uint32_t(ctx.lr));
    }
    __imp__sub_821C5270(ctx, base);
}

// Chooses an object's level of detail (r3 the object, r4 the camera). The measure, at +468, is how big it looks: its size / (tan(field of view) * distance). The level, at +533, is
// the first of the thresholds at +452, +456 and +460 that measure beats (0 the most detailed, parts draw the batches their table at +879 lists for it), and beyond the distance at +464
// the object isn't drawn. --draw-distance F: everything keeps its detail F times as far and is drawn F times as far. AR_LOG_LOD logs how many objects are at each level.
PPC_FUNC_IMPL(__imp__sub_82B67C90);
PPC_FUNC(sub_82B67C90)
{
    float factor = gpu::g_videoSettings.drawDistance;
    uint32_t object = ctx.r3.u32;
    static const bool logLevels = AR_ENV("AR_LOG_LOD") != nullptr;
    if (factor == 1.0f)
        __imp__sub_82B67C90(ctx, base);
    else
    {
        uint32_t saved[4];
        for (uint32_t i = 0; i < 4; i++)
        {
            saved[i] = PPC_LOAD_U32(object + 452 + i * 4);
            float value;
            memcpy(&value, &saved[i], 4);
            value = i == 3 ? value * factor : value / factor;
            uint32_t bits;
            memcpy(&bits, &value, 4);
            PPC_STORE_U32(object + 452 + i * 4, bits);
        }
        __imp__sub_82B67C90(ctx, base);
        for (uint32_t i = 0; i < 4; i++)
            PPC_STORE_U32(object + 452 + i * 4, saved[i]);
    }
    if (logLevels)
    {
        // Each object once, and again when its level changes.
        {
            static std::mutex mutex;
            static std::unordered_map<uint32_t, int> known;
            int current = int(int8_t(PPC_LOAD_U8(object + 533)));
            std::lock_guard lock(mutex);
            auto [entry, added] = known.try_emplace(object, current);
            if (added || entry->second != current)
            {
                entry->second = current;
                auto number = [&](uint32_t offset)
                    {
                        uint32_t bits = PPC_LOAD_U32(object + offset);
                        float value;
                        memcpy(&value, &bits, 4);
                        return value;
                    };
                LOGF_INFO("LODOBJ {:08X} frame {} level {} min {} flags {:08X} thresholds {:.4f} {:.4f} {:.4f} cutoff {:.1f} measure {:.4f} parts {}", object, gpu::SubmittedSwaps(), current, int(int8_t(PPC_LOAD_U8(object + 532))),
                    PPC_LOAD_U32(object + 448), number(452), number(456), number(460), number(464), number(468), PPC_LOAD_U16(object + 508));
            }
        }
        static std::atomic<uint32_t> levels[6];
        static std::atomic<int64_t> lastLog;
        int level = int(int8_t(PPC_LOAD_U8(object + 533)));
        levels[std::clamp(level + 1, 0, 5)]++;
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = lastLog.load();
        if (now - last >= 5 && lastLog.compare_exchange_strong(last, now))
        {
            LOGF_INFO("LOD hidden {} level0 {} level1 {} level2 {} level3 {}", levels[0].exchange(0), levels[1].exchange(0), levels[2].exchange(0), levels[3].exchange(0), levels[4].exchange(0));
        }
    }
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

// Menu rows for the mouse: the highlight behind a row is a sprite of this colour, scaled horizontally to 1 when the row is
// selected and to 0 otherwise. Rows of pages waiting off screen are left out.
static void NoteMenuSprite(uint32_t item, uint8_t* base, int64_t frame)
{
    static std::vector<kbm::MenuRow> rows;
    // The pause menu and windows popping up over it: rows between thin lines, and a selector moving over them. In the order drawn,
    // so the frontmost list stays last.
    struct LineList
    {
        float x, width;
        std::vector<float> lines;
        bool hasSelector = false;
        float selectorX = 0.0f, selectorY = 0.0f;
    };
    static std::vector<LineList> lists;
    struct Selector
    {
        float x, y, width;
    };
    static std::vector<Selector> selectors;
    static int64_t rowsFrame = -1;
    if (frame != rowsFrame)
    {
        for (const Selector& selector : selectors)
        {
            for (LineList& list : lists)
            {
                if (selector.x <= list.x + 2.0f && selector.x + selector.width >= list.x + list.width - 2.0f && std::abs(selector.width - list.width) < 40.0f)
                {
                    list.hasSelector = true;
                    list.selectorX = selector.x;
                    list.selectorY = selector.y;
                }
            }
        }
        for (LineList& list : lists)
        {
            if (!list.hasSelector || list.lines.size() < 2)
                continue;
            std::sort(list.lines.begin(), list.lines.end());
            list.lines.erase(std::unique(list.lines.begin(), list.lines.end()), list.lines.end());
            for (size_t i = 0; i + 1 < list.lines.size(); i++)
            {
                bool selected = list.selectorY >= list.lines[i] - 20.0f && list.selectorY < list.lines[i + 1] - 20.0f;
                rows.push_back({ list.selectorX, list.lines[i], list.x + list.width, list.lines[i + 1], selected, true });
            }
        }
        kbm::SetMenuRows(rows);
        rows.clear();
        lists.clear();
        selectors.clear();
        rowsFrame = frame;
    }

    auto floatAt = [&](uint32_t address)
        {
            uint32_t bits = PPC_LOAD_U32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        };
    uint32_t vertices = PPC_LOAD_U32(item + 36 * 4);
    if (vertices == 0 || PPC_LOAD_U32(item + 34 * 4) != 36 || PPC_LOAD_U32(item + 35 * 4) != 4)
        return;
    float red = floatAt(vertices + 5 * 4), green = floatAt(vertices + 6 * 4), blue = floatAt(vertices + 7 * 4);
    float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX;
    for (uint32_t i = 0; i < 4; i++)
    {
        float x = floatAt(vertices + i * 36), y = floatAt(vertices + i * 36 + 4);
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    float tx = floatAt(item + 16 * 4), ty = floatAt(item + 17 * 4), scale = floatAt(item + 4 * 4);
    if (x1 - x0 >= 80.0f && std::abs(y1 - y0 - 42.0f) < 2.0f)
        selectors.push_back({ tx, ty, x1 - x0 });
    bool highlightColor = std::abs(red - 0.341f) <= 0.01f && std::abs(green - 0.678f) <= 0.01f && blue == 0.0f;
    if (!highlightColor)
        return;
    if (x1 - x0 >= 60.0f && y1 - y0 <= 3.0f)
    {
        LineList* list = nullptr;
        for (LineList& candidate : lists)
            list = std::abs(candidate.x - tx) < 1.0f && std::abs(candidate.width - (x1 - x0)) < 1.0f ? &candidate : list;
        if (!list)
        {
            lists.push_back({ tx, x1 - x0 });
            list = &lists.back();
        }
        list->lines.push_back(ty);
        return;
    }
    if (x1 - x0 < 200.0f || y1 - y0 < 16.0f || y1 - y0 > 60.0f)
        return;
    if (tx + x1 < 0.0f || tx + x0 > 1280.0f)
        return;
    // The highlight grows over a few frames. A row is selected once fully grown or while growing; the UI can update more than
    // once per frame, so a row remembers which way it last changed.
    struct RowScale
    {
        float scale;
        int direction;
    };
    static std::map<uint32_t, RowScale> rowScales;
    uint32_t rowKey = uint32_t(int32_t(tx)) * 4096u + uint32_t(int32_t(ty));
    auto [entry, added] = rowScales.try_emplace(rowKey, RowScale{ scale, 0 });
    if (scale > entry->second.scale)
        entry->second.direction = 1;
    else if (scale < entry->second.scale)
        entry->second.direction = -1;
    entry->second.scale = scale;
    rows.push_back({ tx + x0, ty + y0, tx + x1, ty + y1, scale >= 0.999f || (entry->second.direction > 0 && scale > 0.0f) });
}

// The prompts for breaking out of a stun: a stick on the left, four buttons on the right. With the keyboard and mouse in use the
// groups change places: the buttons go left and show W, A, S and D (the icons' stun version, see GetTextureUncached), and the stick
// goes right, standing still, and shows the mouse with both buttons lit. The sprites are made anew each frame, so moving them
// here doesn't add up. They are recognised by the texture coordinates and sizes of their quads.
static void RearrangeStunPrompt(uint32_t item, uint8_t* base, int64_t frame)
{
    static int64_t stickSeen = -100;
    static float stickX, stickY, buttonsX, buttonsY, buttonsNextX, buttonsNextY;
    static int buttonsNext;
    static int64_t lastFrame = -1;
    if (frame != lastFrame)
    {
        lastFrame = frame;
        if (buttonsNext > 0)
        {
            buttonsX = buttonsNextX / float(buttonsNext);
            buttonsY = buttonsNextY / float(buttonsNext);
        }
        buttonsNextX = buttonsNextY = 0.0f;
        buttonsNext = 0;
        kbm::SetStunPromptVisible(frame - stickSeen < 5);
    }

    uint32_t vertices = PPC_LOAD_U32(item + 36 * 4);
    if (vertices == 0 || PPC_LOAD_U32(item + 34 * 4) != 36 || PPC_LOAD_U32(item + 35 * 4) != 4)
        return;
    auto floatAt = [&](uint32_t address)
        {
            uint32_t bits = PPC_LOAD_U32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        };
    auto setFloat = [&](uint32_t address, float value)
        {
            uint32_t bits;
            memcpy(&bits, &value, 4);
            PPC_STORE_U32(address, bits);
        };
    float half = std::abs(floatAt(vertices)), u = floatAt(vertices + 3 * 4), v = floatAt(vertices + 4 * 4);
    auto isUv = [&](float wantedU, float wantedV) { return std::abs(u - wantedU) < 0.006f && std::abs(v - wantedV) < 0.006f; };
    auto isSize = [&](float wanted) { return std::abs(half - wanted) < 1.5f; };
    bool pulse = isUv(0.502f, 0.348f), disc = isUv(0.004f, 0.793f), stripes = isUv(0.566f, 0.691f);
    bool stickIcon = isUv(0.246f, 0.004f) && isSize(27.0f);
    bool buttonIcon = isSize(20.0f) && (isUv(0.520f, 0.238f) || isUv(0.348f, 0.238f) || isUv(0.473f, 0.004f) || isUv(0.645f, 0.004f));
    bool stickPart = stickIcon || (pulse && isSize(32.0f)) || (disc && (isSize(32.0f) || isSize(27.0f) || isSize(21.0f))) ||
        (stripes && (isSize(32.0f) || isSize(27.0f)));
    bool buttonPart = buttonIcon || (pulse && isSize(18.0f)) || ((disc || stripes) && isSize(13.0f));
    float tx = floatAt(item + 16 * 4), ty = floatAt(item + 17 * 4);
    // Their rings come from another atlas, and the HUD has the same ones: those of the prompts are drawn right after the other parts, in the same place.
    // A pressed button's icon has a second copy that changes size, so it goes by the place too.
    static float lastStickPartX, lastStickPartY, lastButtonPartX, lastButtonPartY;
    auto isAt = [&](float x, float y) { return std::abs(tx - x) < 3.0f && std::abs(ty - y) < 3.0f; };
    if (isSize(32.0f) && (isUv(0.435f, 0.352f) || isUv(0.255f, 0.430f)))
    {
        buttonPart = isAt(lastButtonPartX, lastButtonPartY);
        stickPart = !buttonPart && isAt(lastStickPartX, lastStickPartY);
    }
    else if (!stickPart && !buttonPart && half > 8.0f && half < 40.0f)
    {
        if (isUv(0.520f, 0.238f) || isUv(0.348f, 0.238f) || isUv(0.473f, 0.004f) || isUv(0.645f, 0.004f) || pulse || disc || stripes)
            buttonPart = isAt(lastButtonPartX, lastButtonPartY);
        if (!buttonPart && (isUv(0.246f, 0.004f) || pulse || disc || stripes))
            stickPart = isAt(lastStickPartX, lastStickPartY);
    }
    if (!stickPart && !buttonPart)
        return;
    (stickPart ? lastStickPartX : lastButtonPartX) = tx;
    (stickPart ? lastStickPartY : lastButtonPartY) = ty;

    if (stickIcon)
        stickSeen = frame;
    if (disc && isSize(32.0f))
    {
        stickX = tx;
        stickY = ty;
    }
    if (buttonIcon)
    {
        buttonsNextX += tx;
        buttonsNextY += ty;
        buttonsNext++;
    }
    if (!kbm::Active() || frame - stickSeen >= 5 || buttonsX == 0.0f)
        return;

    if (buttonPart)
    {
        setFloat(item + 16 * 4, tx + stickX - buttonsX);
        setFloat(item + 17 * 4, ty + stickY - buttonsY);
    }
    else
    {
        setFloat(item + 16 * 4, buttonsX);
        setFloat(item + 17 * 4, buttonsY);
        // The stick's trailing rings become rings pulsing outward, as fast as the buttons'.
        if (pulse)
        {
            static const auto start = std::chrono::steady_clock::now();
            float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            float phase = time * 7.0f + floatAt(item + 29 * 4);
            float scale = 1.0f + 0.5f * (phase - std::floor(phase));
            setFloat(item + 4 * 4, scale);
            setFloat(item + 9 * 4, scale);
        }
    }
}

static std::atomic<bool> g_uiDumpRequest, g_uiTextDumpRequest;

void RequestUiDump()
{
    g_uiDumpRequest = true;
    g_uiTextDumpRequest = true;
}

PPC_FUNC_IMPL(__imp__sub_8297F6E0);

// The Gallery's character viewer, each update (r3: its object): the mouse turns the character and tilts the camera, the same things the right
// stick does there, without going through a stick's dead zone and curve. The tilt is limited as the title limits it, in degrees at +336 and +340.
PPC_FUNC_IMPL(__imp__sub_82E1C508);
PPC_FUNC_IMPL(__imp__sub_8270B3C8);
PPC_FUNC_IMPL(__imp__sub_826F5EF8);
PPC_FUNC_IMPL(__imp__sub_826F5E90);
PPC_FUNC(sub_82E1C508)
{
    kbm::NoteViewer();
    float turn, tilt;
    if (kbm::TakeViewerMouse(turn, tilt))
    {
        static constexpr uint32_t CAMERA = 0x831C51C0;
        auto loadFloat = [&](uint32_t address)
            {
                uint32_t bits = PPC_LOAD_U32(address);
                float value;
                memcpy(&value, &bits, 4);
                return value;
            };
        uint32_t viewer = ctx.r3.u32, character = PPC_LOAD_U32(0x831C3110);
        PPCContext saved = ctx;
        if (character != 0 && turn != 0.0f)
        {
            ctx.r3.u64 = character;
            ctx.f1.f64 = turn;
            __imp__sub_8270B3C8(ctx, base);
            ctx = saved;
        }
        if (tilt != 0.0f)
        {
            static constexpr float RADIANS = 3.14159265f / 180.0f;
            float value = std::clamp(loadFloat(CAMERA + 608) + tilt, loadFloat(viewer + 336) * RADIANS, loadFloat(viewer + 340) * RADIANS);
            uint32_t bits;
            memcpy(&bits, &value, 4);
            PPC_STORE_U32(CAMERA + 608, bits);
            ctx.r3.u64 = CAMERA + 304;
            __imp__sub_826F5EF8(ctx, base);
            ctx = saved;
            ctx.r3.u64 = CAMERA + 304;
            __imp__sub_826F5E90(ctx, base);
            ctx = saved;
        }
    }
    __imp__sub_82E1C508(ctx, base);
}

// The speed of a camera (r3: 0 normal, otherwise aiming), which the title asks for each update after setting its other settings.
PPC_FUNC_IMPL(__imp__sub_82E5EFC8);
PPC_FUNC(sub_82E5EFC8)
{
    bool aiming = int16_t(ctx.r3.u32) != 0;
    __imp__sub_82E5EFC8(ctx, base);
    float speed;
    if (kbm::MouseCameraSpeed(aiming, speed))
        ctx.f1.f64 = speed;
}

// The sounds of the rows added to Options: sub_8297F6E0(the sounds of the menus, id, 0), as its menus do.
static void PlayOptionsSounds(PPCContext& __restrict ctx, uint8_t* base)
{
    for (int sound = kbm::TakeOptionsSound(); sound >= 0; sound = kbm::TakeOptionsSound())
    {
        PPCContext saved = ctx;
        ctx.r3.u64 = 0x820A5F58;
        ctx.r4.u64 = uint64_t(sound);
        ctx.r5.u64 = 0;
        __imp__sub_8297F6E0(ctx, base);
        ctx = saved;
    }
}

// The title updates its Options screen: from the pause menu.
PPC_FUNC_IMPL(__imp__sub_827F2370);
PPC_FUNC(sub_827F2370)
{
    kbm::NoteOptionsUpdate();
    __imp__sub_827F2370(ctx, base);
    PlayOptionsSounds(ctx, base);
}

// The main menu each update (r3 = its object, +316 its state: 22 on the Options screen).
PPC_FUNC_IMPL(__imp__sub_82E29220);
PPC_FUNC(sub_82E29220)
{
    bool options = PPC_LOAD_U32(ctx.r3.u32 + 316) == 22;
    if (options)
        kbm::NoteOptionsUpdate();
    __imp__sub_82E29220(ctx, base);
    if (options)
        PlayOptionsSounds(ctx, base);
}

// The Options screen with the rows added to it (see keyboard_mouse.cpp): what the title draws of its rows moved down, its sprites for
// a row's background and highlight drawn again for the added rows, so they look the same. Returns whether to draw the item.
PPC_FUNC_IMPL(__imp__sub_8280FA08);

// Ultrawide (--ultrawide). The picture stays 16:9 inside and is shown wider, so it takes a wider view, and everything drawn flat on it comes out wider too: the
// UI is narrowed by as much, about the middle, which makes it the right shape in the middle of the screen. A draw's x becomes x * scale + offset.
static void DrawUiItem(PPCContext& __restrict ctx, uint8_t* base, float scale, float offset, void (*draw)(PPCContext& __restrict, uint8_t*))
{
    if (scale == 1.0f && offset == 0.0f)
    {
        draw(ctx, base);
        return;
    }
    uint32_t item = ctx.r3.u32;
    static constexpr uint32_t COLUMN[4] = { 4, 8, 12, 16 };
    uint32_t saved[4];
    for (int i = 0; i < 4; i++)
    {
        saved[i] = PPC_LOAD_U32(item + COLUMN[i] * 4);
        float value;
        memcpy(&value, &saved[i], 4);
        value = value * scale + (i == 3 ? offset : 0.0f);
        uint32_t bits;
        memcpy(&bits, &value, 4);
        PPC_STORE_U32(item + COLUMN[i] * 4, bits);
    }
    draw(ctx, base);
    for (int i = 0; i < 4; i++)
        PPC_STORE_U32(item + COLUMN[i] * 4, saved[i]);
}

static void DrawUiSprite(PPCContext& __restrict ctx, uint8_t* base)
{
    float wider = gpu::UltrawideFactor();
    if (wider == 1.0f)
    {
        __imp__sub_8280FA08(ctx, base);
        return;
    }
    float scale = 1.0f / wider, offset = 640.0f - 640.0f / wider;
    // Its vertices (x and y first, stride in bytes at word 34, count at 35, address at 36) and transform.
    uint32_t item = ctx.r3.u32;
    uint32_t vertices = PPC_LOAD_U32(item + 36 * 4), stride = PPC_LOAD_U32(item + 34 * 4), count = PPC_LOAD_U32(item + 35 * 4);
    auto loadFloat = [&](uint32_t address)
        {
            uint32_t bits = PPC_LOAD_U32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        };
    if (vertices != 0 && stride >= 8 && count != 0 && count <= 256)
    {
        float lowX = FLT_MAX, highX = -FLT_MAX, lowY = FLT_MAX, highY = -FLT_MAX;
        for (uint32_t i = 0; i < count; i++)
        {
            float x = loadFloat(vertices + i * stride), y = loadFloat(vertices + i * stride + 4);
            lowX = std::min(lowX, x);
            highX = std::max(highX, x);
            lowY = std::min(lowY, y);
            highY = std::max(highY, y);
        }
        float scaleX = loadFloat(item + 4 * 4), scaleY = loadFloat(item + 9 * 4), translate = loadFloat(item + 16 * 4);
        float left = translate + std::min(lowX * scaleX, highX * scaleX), right = translate + std::max(lowX * scaleX, highX * scaleX);
        float height = std::abs(highY - lowY) * std::abs(scaleY);
        bool reachesLeft = left <= 1.0f, reachesRight = right >= 1279.0f;
        if (right > left)
        {
            if (right - left >= 1180.0f || ((reachesLeft || reachesRight) && right - left >= 400.0f && height >= 300.0f))
            {
                // A background (dimming and vignettes come in halves): as wide as the screen stays so.
                scale = 1.0f;
                offset = 0.0f;
            }
            else if (reachesLeft != reachesRight)
            {
                // A strip from the edge of the screen goes on reaching it, its other end where the UI is.
                float newLeft = reachesLeft ? left : 640.0f + (left - 640.0f) / wider;
                float newRight = reachesRight ? right : 640.0f + (right - 640.0f) / wider;
                scale = (newRight - newLeft) / (right - left);
                offset = newLeft - scale * left;
            }
        }
    }
    DrawUiItem(ctx, base, scale, offset, __imp__sub_8280FA08);
}

// Builds a camera's projection (f1 the field of view, f2 the shape, from the screen's size). A wider shape for a wider view.
static std::atomic<int64_t> g_cameraBuiltTime{ -100000 };

static int64_t NowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float gpu::UltrawideFactor()
{
    float setting = gpu::UltrawideSetting();
    return setting != 1.0f && NowMilliseconds() - g_cameraBuiltTime.load() < 300 ? setting : 1.0f;
}

PPC_FUNC_IMPL(__imp__sub_82703DF8);
PPC_FUNC(sub_82703DF8)
{
    if (std::abs(float(ctx.f2.f64) - 16.0f / 9.0f) < 0.01f)
    {
        g_cameraBuiltTime = NowMilliseconds();
        ctx.f2.f64 = double(float(ctx.f2.f64) * gpu::UltrawideSetting());
    }
    __imp__sub_82703DF8(ctx, base);
}
static bool OptionsSprite(PPCContext& __restrict ctx, uint8_t* base, int64_t frame)
{
    if (!kbm::OptionsLayoutActive())
        return true;
    uint32_t item = ctx.r3.u32;
    uint32_t vertices = PPC_LOAD_U32(item + 36 * 4);
    if (vertices == 0 || PPC_LOAD_U32(item + 34 * 4) != 36 || PPC_LOAD_U32(item + 35 * 4) != 4)
        return true;
    auto floatAt = [&](uint32_t address)
        {
            uint32_t bits = PPC_LOAD_U32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        };
    auto setFloat = [&](uint32_t address, float value)
        {
            uint32_t bits;
            memcpy(&bits, &value, 4);
            PPC_STORE_U32(address, bits);
        };
    float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX;
    for (uint32_t i = 0; i < 4; i++)
    {
        float x = floatAt(vertices + i * 36), y = floatAt(vertices + i * 36 + 4);
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    float tx = floatAt(item + 16 * 4), ty = floatAt(item + 17 * 4), width = x1 - x0, height = y1 - y0;
    float center = ty + (y0 + y1) * 0.5f;
    if (center < 193.0f || center >= 600.0f)
        return true;
    float red = floatAt(vertices + 5 * 4), green = floatAt(vertices + 6 * 4);
    bool background = std::abs(tx - 88.0f) < 1.0f && std::abs(width - 416.0f) < 1.0f && std::abs(height - 28.0f) < 1.0f;
    bool highlight = std::abs(tx - 88.0f) < 1.0f && std::abs(width - 414.0f) < 1.0f && std::abs(height - 28.0f) < 1.0f && std::abs(red - 0.341f) < 0.01f && std::abs(green - 0.678f) < 0.01f;
    bool small = tx >= 300.0f && tx < 620.0f && width <= 24.0f && height <= 24.0f;
    if (!background && !highlight && !small)
        return true;

    int extra = kbm::OptionsExtraRow(), line = kbm::OptionsMouseLine();
    bool mouseOpen = line >= 0 && extra >= 0;
    float moved = ty + kbm::OptionsShift(center);
    setFloat(item + 17 * 4, moved);
    if (small)
    {
        // Values aren't shown beside a row opened, and those of a row the title thinks is selected are the wrong colour.
        if (mouseOpen)
            return false;
        return !(extra >= 0 && !kbm::OptionsGameOpen() && std::abs(center - kbm::OptionsGameRowY(kbm::OptionsGameRow())) < 14.0f);
    }

    auto drawAgain = [&](float centerY, float scale)
        {
            setFloat(item + 17 * 4, centerY);
            setFloat(item + 4 * 4, scale);
            ctx.r3.u64 = item;
            DrawUiSprite(ctx, base);
        };
    static int64_t backgroundFrame = -1, highlightFrame = -1;
    if (background)
    {
        if (backgroundFrame == frame)
            return true;
        backgroundFrame = frame;
        DrawUiSprite(ctx, base);
        for (int i = 0; i < 4; i++)
        {
            bool opened = mouseOpen && extra == i;
            drawAgain(kbm::OptionsExtraRowY(i), opened ? kbm::OptionsOpenScale() : 1.0f);
            if (opened)
            {
                drawAgain(kbm::OptionsExtraRowY(i) + 28.0f, kbm::OptionsOpenScale());
                drawAgain(kbm::OptionsExtraRowY(i) + 56.0f, kbm::OptionsOpenScale());
            }
        }
        return false;
    }

    // AR_LOG_OPTIONS: how its highlights grow.
    static const bool logHighlights = AR_ENV("AR_LOG_OPTIONS") != nullptr;
    if (logHighlights && floatAt(item + 4 * 4) > 0.0f && floatAt(item + 4 * 4) < 0.999f)
        LOGF_INFO("options highlight {} {:.3f} frame {}", ty, floatAt(item + 4 * 4), frame);
    // The title's highlight doesn't show on the row it thinks is selected, while an added row is, but shrinks and grows as usual.
    if (kbm::OptionsGameHighlightLimit() < 1.0f)
        setFloat(item + 4 * 4, std::min(floatAt(item + 4 * 4), kbm::OptionsGameHighlightLimit()));
    if (highlightFrame == frame)
        return true;
    highlightFrame = frame;
    DrawUiSprite(ctx, base);
    for (int i = 0; i < 4; i++)
    {
        float grown = kbm::OptionsExtraHighlight(i);
        if (grown <= 0.0f)
            continue;
        if (mouseOpen && extra == i)
            drawAgain(kbm::OptionsExtraRowY(i) + 28.0f * float(line), grown * kbm::OptionsOpenScale() * 416.0f / 414.0f);
        else
            drawAgain(kbm::OptionsExtraRowY(i), grown * 416.0f / 414.0f);
    }
    return false;
}

// The same for its text. Returns whether to draw it.
static bool OptionsText(uint32_t item, uint8_t* base)
{
    if (!kbm::OptionsLayoutActive())
        return true;
    auto floatAt = [&](uint32_t address)
        {
            uint32_t bits = PPC_LOAD_U32(address);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        };
    auto setFloat = [&](uint32_t address, float value)
        {
            uint32_t bits;
            memcpy(&bits, &value, 4);
            PPC_STORE_U32(address, bits);
        };
    float x = floatAt(item + 16 * 4), y = floatAt(item + 17 * 4);
    // How far the screen has faded in: its big title's text (a row opened dims the rows). A faint copy of the title follows it, drifting
    // up and down over the same place, which isn't that: the title comes first each frame.
    static int64_t alphaFrame = -1;
    int64_t frame = gpu::SubmittedSwaps();
    if (frame != alphaFrame && std::abs(x - 76.0f) < 1.0f && std::abs(y - 150.0f) < 1.0f)
    {
        alphaFrame = frame;
        kbm::SetOptionsTextAlpha(floatAt(item + 25 * 4));
    }
    if (y < 193.0f || y >= 620.0f)
        return true; // The title, the footer.
    int extra = kbm::OptionsExtraRow();
    if (x > 600.0f)
        return extra < 0; // The description is about the title's row.
    bool label = std::abs(x - 96.0f) < 1.0f;
    // The cameras' labels: the overlay draws their new names.
    if (label && (std::abs(y - kbm::OptionsGameRowY(2)) < 1.0f || std::abs(y - kbm::OptionsGameRowY(3)) < 1.0f))
        return false;
    if (kbm::OptionsMouseLine() >= 0)
    {
        // A mouse row opened: the rest of the list dims, without values, as for the title's own.
        if (!label)
            return false;
        setFloat(item + 25 * 4, floatAt(item + 25 * 4) * 0.4f);
    }
    // The row the title thinks is selected isn't as far as anyone can see.
    if (extra >= 0 && !kbm::OptionsGameOpen() && std::abs(y - kbm::OptionsGameRowY(kbm::OptionsGameRow())) < 1.0f)
    {
        uint32_t grey = 0x3F60E000;
        for (uint32_t i = 22; i < 25; i++)
            PPC_STORE_U32(item + i * 4, grey);
    }
    setFloat(item + 17 * 4, y + kbm::OptionsShift(y));
    return true;
}

// The UI's sprite draw (a render queue item's draw function, r3 = item). Diagnostics (AR_LOG_UI_ITEMS=<frame>,<count>):
// the item's first 48 words in those frames.
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
    NoteMenuSprite(ctx.r3.u32, base, frame);
    RearrangeStunPrompt(ctx.r3.u32, base, frame);
    // Also on request (remote control "find uidump", or F11 with AR_SCAN_KEY): this frame and the next, with the quad's vertices.
    static int64_t requestedFrame = -1;
    if (g_uiDumpRequest.exchange(false))
        requestedFrame = frame;
    bool requested = requestedFrame >= 0 && frame >= requestedFrame && frame < requestedFrame + 2;
    if ((enabled && frame >= first && frame < first + frames) || requested)
    {
        std::string text;
        for (uint32_t i = 0; i < 48; i++)
            text += fmt::format(" {:08X}", PPC_LOAD_U32(ctx.r3.u32 + i * 4));
        LOGF_INFO("UI {} {:08X}:{}", frame, ctx.r3.u32, text);
        if (requested)
        {
            uint32_t vertices = PPC_LOAD_U32(ctx.r3.u32 + 36 * 4), stride = PPC_LOAD_U32(ctx.r3.u32 + 34 * 4), count = PPC_LOAD_U32(ctx.r3.u32 + 35 * 4);
            if (vertices != 0 && stride == 36 && count <= 64)
            {
                std::string data;
                for (uint32_t i = 0; i < std::min(count, 4u) * 9; i++)
                    data += fmt::format(" {:08X}", PPC_LOAD_U32(vertices + i * 4));
                LOGF_INFO("UIV {} {:08X}:{}", frame, ctx.r3.u32, data);
            }
        }
    }
    if (OptionsSprite(ctx, base, frame))
        DrawUiSprite(ctx, base);
}

// A UI text item's draw (r3 = item). Diagnostics: dumped with the sprites.
PPC_FUNC_IMPL(__imp__sub_82B7E680);
PPC_FUNC(sub_82B7E680)
{
    static int64_t requestedFrame = -1;
    int64_t frame = gpu::SubmittedSwaps();
    if (g_uiTextDumpRequest.exchange(false))
        requestedFrame = frame;
    // AR_LOG_UI_TEXT=<frame>,<count>: in those frames too.
    static int64_t logFirst = -1, logFrames = 0;
    static const bool logRange = []
        {
            const char* text = AR_ENV("AR_LOG_UI_TEXT");
            if (!text)
                return false;
            logFirst = atoll(text);
            const char* comma = strchr(text, ',');
            logFrames = comma ? atoll(comma + 1) : 1;
            return true;
        }();
    if ((requestedFrame >= 0 && frame >= requestedFrame && frame < requestedFrame + 1) || (logRange && frame >= logFirst && frame < logFirst + logFrames))
    {
        std::string text;
        for (uint32_t i = 0; i < 40; i++)
            text += fmt::format(" {:08X}", PPC_LOAD_U32(ctx.r3.u32 + i * 4));
        LOGF_INFO("UIT {} {:08X}:{}", frame, ctx.r3.u32, text);
    }

    // The loading screen's "LS + RS  Unleash Rampage" is three items in one place: icon, plus sign, icon. With the keyboard in use both stick clicks are Q, so
    // only the last stays.
    if (kbm::Active())
    {
        uint32_t xBits = PPC_LOAD_U32(ctx.r3.u32 + 16 * 4), yBits = PPC_LOAD_U32(ctx.r3.u32 + 17 * 4);
        float x, y;
        memcpy(&x, &xBits, 4);
        memcpy(&y, &yBits, 4);
        if (std::abs(x - 598.0f) < 1.0f && std::abs(y - 526.0f) < 1.0f)
        {
            static int64_t rampageFrame = -1;
            static int rampageItems;
            if (frame != rampageFrame)
            {
                rampageFrame = frame;
                rampageItems = 0;
            }
            if (rampageItems++ < 2)
                return;
        }
    }

    // The title screen's prompt, in black on its strip, which comes in from the left: replaced.
    {
        uint32_t item = ctx.r3.u32;
        uint32_t xBits = PPC_LOAD_U32(item + 16 * 4), yBits = PPC_LOAD_U32(item + 17 * 4), alphaBits = PPC_LOAD_U32(item + 25 * 4);
        float x, y, alpha;
        memcpy(&x, &xBits, 4);
        memcpy(&y, &yBits, 4);
        memcpy(&alpha, &alphaBits, 4);
        if (std::abs(y - 604.0f) < 4.0f && x <= 70.0f && PPC_LOAD_U32(item + 22 * 4) == 0 && (PPC_LOAD_U32(0x83214CF0) & 0x02000000) != 0 && kbm::ReplaceTitlePrompt(x, y, alpha))
            return;
    }
    if (!OptionsText(ctx.r3.u32, base))
        return;
    float wider = gpu::UltrawideFactor();
    DrawUiItem(ctx, base, 1.0f / wider, 640.0f - 640.0f / wider, __imp__sub_82B7E680);
}

void WatchGuestWords(const std::vector<uint32_t>& addresses, bool reads = false);

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
