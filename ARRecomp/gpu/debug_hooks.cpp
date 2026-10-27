#include <stdafx.h>
#include <kernel/memory.h>

// Diagnostics for D3D occlusion queries (AR_LOG_QUERIES): how long IDirect3DQuery9::GetData keeps reporting a
// query as pending, with the query's fence and the device's fence progress.
PPC_FUNC_IMPL(__imp__sub_8223F950);
PPC_FUNC(sub_8223F950)
{
    static const bool enabled = getenv("AR_LOG_QUERIES") != nullptr;
    uint32_t query = ctx.r3.u32;
    __imp__sub_8223F950(ctx, base);
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
