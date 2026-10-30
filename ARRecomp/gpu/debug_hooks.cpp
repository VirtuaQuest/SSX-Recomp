#include <stdafx.h>
#include <kernel/memory.h>

// IDirect3DQuery9::GetData. Diagnostics for D3D occlusion queries (AR_LOG_QUERIES): how long it keeps reporting a
// query as pending, with the query's fence and the device's fence progress.
PPC_FUNC_IMPL(__imp__sub_8223F950);
PPC_FUNC(sub_8223F950)
{
    static const bool enabled = AR_ENV("AR_LOG_QUERIES") != nullptr;
    uint32_t query = ctx.r3.u32;
    uint32_t output = ctx.r4.u32, outputSize = ctx.r5.u32;
    __imp__sub_8223F950(ctx, base);

    // Occlusion queries aren't measured: the command processor advances the sample counter by a fixed amount, so a
    // completed query always returns 4096 samples. The title polls GetData until the GPU reaches the query (several
    // milliseconds a frame), so pending occlusion queries are answered with that result right away instead
    // (AR_WAIT_QUERIES=1 keeps the polling).
    static const bool waitQueries = AR_ENV("AR_WAIT_QUERIES") != nullptr;
    if (!waitQueries && ctx.r3.u32 == 1 && PPC_LOAD_U32(query + 4) == 9 && outputSize == 4)
    {
        PPC_STORE_U32(output, 4096);
        ctx.r3.u64 = 0;
    }

    // AR_LOG_QUERY_RESULTS: what completed queries return.
    static const bool logResults = AR_ENV("AR_LOG_QUERY_RESULTS") != nullptr;
    if (logResults && ctx.r3.u32 == 0)
    {
        static int logged;
        if (logged++ < 40)
            LOGF_INFO("query 0x{:08X} type {} result {} size {}: {} {} {}", query, PPC_LOAD_U32(query + 4), PPC_LOAD_U32(output), outputSize,
                outputSize >= 16 ? PPC_LOAD_U32(output + 4) : 0, outputSize >= 16 ? PPC_LOAD_U32(output + 8) : 0,
                outputSize >= 16 ? PPC_LOAD_U32(output + 12) : 0);
    }
    if (!enabled)
        return;

    static thread_local uint32_t pendingQuery;
    static thread_local std::chrono::steady_clock::time_point pendingSince;
    static thread_local uint32_t pendingCalls;
    auto now = std::chrono::steady_clock::now();

    if (ctx.r3.u32 == 1)
    {
        if (pendingQuery != query)
        {
            pendingQuery = query;
            pendingSince = now;
            pendingCalls = 0;
        }
        pendingCalls++;
        return;
    }

    if (pendingQuery == query)
    {
        double waited = std::chrono::duration<double, std::milli>(now - pendingSince).count();
        // Summary every 5 seconds: how long the title spent waiting for query results.
        static std::mutex mutex;
        static double total, longest;
        static uint32_t waits;
        static auto lastSummary = now;
        {
            std::lock_guard lock(mutex);
            total += waited;
            longest = std::max(longest, waited);
            waits++;
            if (now - lastSummary > std::chrono::seconds(5))
            {
                LOGF_INFO("query waits: {} in {:.1f} s, {:.0f} ms total ({:.1f} ms average, {:.1f} ms longest)", waits,
                    std::chrono::duration<double>(now - lastSummary).count(), total, total / waits, longest);
                lastSummary = now;
                total = longest = 0.0;
                waits = 0;
            }
        }
        static int logged;
        if (waited > 5.0 && logged++ < 40)
        {
            uint32_t device = PPC_LOAD_U32(query);
            uint32_t completedAddress = PPC_LOAD_U32(device + 11024);
            LOGF_INFO("query 0x{:08X} pending {:.1f} ms ({} calls): query fence {}, device fence {}, completed {}", query, waited,
                pendingCalls, PPC_LOAD_U32(query + 24), PPC_LOAD_U32(device + 11036), PPC_LOAD_U32(completedAddress));
        }
        pendingQuery = 0;
    }
}

// Diagnostics for the game's backwards LZ decompressor (AR_DUMP_DECOMPRESS=<hex guest address>): saves the input
// and output of calls whose output covers the address. Arguments: r4/r5 source and size, r6/r7 destination and size.
PPC_FUNC_IMPL(__imp__sub_82570808);
PPC_FUNC(sub_82570808)
{
    static const uint32_t watched = AR_ENV("AR_DUMP_DECOMPRESS") ? uint32_t(strtoul(AR_ENV("AR_DUMP_DECOMPRESS"), nullptr, 16)) : 0;
    uint32_t source = ctx.r4.u32, sourceSize = ctx.r5.u32, dest = ctx.r6.u32, destSize = ctx.r7.u32;
    bool dump = watched != 0 && watched >= dest && watched - dest < destSize;
    static int index;
    int call = index;
    if (dump)
    {
        index++;
        std::ofstream file(fmt::format("decompress_{}_in.bin", call), std::ios::binary);
        file.write(reinterpret_cast<const char*>(g_memory.Translate(source)), sourceSize);
    }

    __imp__sub_82570808(ctx, base);

    if (dump)
    {
        std::ofstream file(fmt::format("decompress_{}_out.bin", call), std::ios::binary);
        file.write(reinterpret_cast<const char*>(g_memory.Translate(dest)), destSize);
        LOGF_INFO("decompress {}: 0x{:08X} ({} bytes) to 0x{:08X} ({} bytes) returned {}", call, source, sourceSize, dest, destSize, ctx.r3.u32);
    }
}

void PrintCurrentStack(int maxFrames);

// Diagnostics for save data errors (AR_LOG_SAVE_ERRORS): the save task's result code and where it comes from.
PPC_FUNC_IMPL(__imp__sub_826E3D88);
PPC_FUNC(sub_826E3D88)
{
    if (AR_ENV("AR_LOG_SAVE_ERRORS"))
    {
        LOGF_INFO("save task 0x{:08X} result {} (r5 0x{:08X} r6 0x{:08X})", ctx.r3.u32, ctx.r4.s32, ctx.r5.u32, ctx.r6.u32);
        PrintCurrentStack(8);
    }
    __imp__sub_826E3D88(ctx, base);
}
