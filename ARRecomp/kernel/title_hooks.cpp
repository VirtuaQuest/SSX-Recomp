// Hooks on SSX's own code. Each has a switch to turn it off.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>

// A rider's update (sub_82964448, rider in r3) reads its state block at +724, whose first word points at the rider's data
// (followed through +180 and +12 to a state number). With the archives on the cache drive, loading is fast enough that
// this update can run while another thread hasn't attached that data yet (the block's first word still 0), and it
// crashed reading through the null pointer. The console's slower CPU never got there first. Such an update is skipped
// until the data is there: the rider stands still for that frame. AR_NO_RIDER_GUARD turns this off.
PPC_FUNC_IMPL(__imp__sub_82964448);
PPC_FUNC(sub_82964448)
{
    static const bool guard = AR_ENV("AR_NO_RIDER_GUARD") == nullptr;
    if (guard)
    {
        uint32_t block = PPC_LOAD_U32(ctx.r3.u32 + 724);
        if (block != 0 && PPC_LOAD_U32(block) == 0)
        {
            static std::atomic<uint32_t> skipped;
            uint32_t count = ++skipped;
            if (count == 1 || count == 100 || count == 10000)
                LOGF_INFO("rider update skipped while its data isn't attached ({} times)", count);
            ctx.r3.u64 = 0;
            return;
        }
    }
    __imp__sub_82964448(ctx, base);
}
