// Hooks on SSX's own code. Each has a switch to turn it off.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>

// A rider's update (sub_82964448) walks state blocks whose first word points at data it reads through +180 and +12 to a
// state number. With the archives on the cache drive, loading is fast enough that another thread can still be attaching
// that data while the update runs (the first word 0), and it crashed reading through the null pointer. The console's
// slower CPU never got there first. A check at the two reads (mid-function hooks in AR.toml) goes on with the next block
// instead, as the update does for blocks it has nothing to do with. AR_NO_RIDER_GUARD turns this off.
bool RiderDataMissingHook(PPCRegister& block)
{
    static const bool guard = AR_ENV("AR_NO_RIDER_GUARD") == nullptr;
    if (!guard || block.u32 == 0 || g_memory.Translate<be<uint32_t>>(block.u32)->get() != 0)
        return false;
    static std::atomic<uint32_t> skipped;
    uint32_t count = ++skipped;
    if (count == 1 || count == 100 || count == 10000)
        LOGF_INFO("rider state block skipped while its data isn't attached ({} times)", count);
    return true;
}
