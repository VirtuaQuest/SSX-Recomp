// Hooks on SSX's own code. Each has a switch to turn it off.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>

// SSX runs periodic tickers (sub_83047120: a 30 Hz and a 60 Hz one) on threads of their own. Each pass adds the time
// since the last (a millisecond tick count) and, between ticks, waits through sub_82356C38: a sleep of the milliseconds
// in the word at 821A80D8, or a bare yield when that's 0, as SSX ships it. On the console those threads had hardware
// threads to themselves; here each spun a host core at 100% (two of a 4 core CPU's, in menus and in races). With the
// word at 1 they sleep a millisecond between checks: the clock they read has that resolution, and the time is added up
// exactly, so ticks keep their rate. AR_NO_TICKER_SLEEP keeps the bare yield.
// AR_LOG_TICKERS logs each ticker: its interval (ns) and tick function.
PPC_FUNC_IMPL(__imp__sub_83047120);
PPC_FUNC(sub_83047120)
{
    static const bool sleep = AR_ENV("AR_NO_TICKER_SLEEP") == nullptr;
    constexpr uint32_t TICKER_SLEEP_MS = 0x821A80D8;
    if (sleep && PPC_LOAD_U32(TICKER_SLEEP_MS) == 0)
        PPC_STORE_U32(TICKER_SLEEP_MS, 1);
    static const bool log = AR_ENV("AR_LOG_TICKERS") != nullptr;
    if (log)
    {
        uint32_t object = ctx.r3.u32, vtable = PPC_LOAD_U32(object);
        LOGF_INFO("ticker {:08X}: vtable {:08X} tick {:08X} interval {} ns", object, vtable, PPC_LOAD_U32(vtable + 32),
            int64_t(PPC_LOAD_U64(object + 72)));
    }
    __imp__sub_83047120(ctx, base);
}
