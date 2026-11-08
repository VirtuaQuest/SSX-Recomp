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

// Offline mode. SSX's online features (Global Events, SSX Rivals) live on EA's servers, which are gone, and everything
// else plays without them. Two things still pointed players at a sign-in that can't happen; AR_NO_OFFLINE_MODE brings
// both back:
// - The main menu's "Connect to EA Server" button (X). sub_825D38A8 publishes the menu's online_pass_data, with
//   showConnectToServerButton set while not connected, through sub_82F948C8(object, name, bool); that one name is
//   published as false.
// - Messages that say to sign in to Xbox LIVE. Text lookups (sub_8256A878(manager, out string, id, flags), out an
//   EASTL string: begin, end, capacity end) of these IDs get text that says what's the case, written over the English
//   original (only when the lookup returned the English text, and only when it fits).
static bool OfflineMode()
{
    static const bool on = AR_ENV("AR_NO_OFFLINE_MODE") == nullptr;
    return on;
}

PPC_FUNC_IMPL(__imp__sub_82F948C8);
PPC_FUNC(sub_82F948C8)
{
    constexpr uint32_t SHOW_CONNECT_TO_SERVER_BUTTON = 0x82081240; // "showConnectToServerButton"
    if (ctx.r4.u32 == SHOW_CONNECT_TO_SERVER_BUTTON && OfflineMode())
        ctx.r5.u64 = 0;
    __imp__sub_82F948C8(ctx, base);
}

// AR_LOG_TEXT logs every text lookup (ID and text).
PPC_FUNC_IMPL(__imp__sub_8256A878);
PPC_FUNC(sub_8256A878)
{
    static const bool log = AR_ENV("AR_LOG_TEXT") != nullptr;
    uint32_t out = ctx.r4.u32, id = ctx.r5.u32;
    __imp__sub_8256A878(ctx, base);

    auto text = [&]
        {
            uint32_t begin = PPC_LOAD_U32(out), end = PPC_LOAD_U32(out + 4);
            std::string value;
            for (uint32_t a = begin; a < end && a < begin + 400; a++)
                value += char(PPC_LOAD_U8(a));
            return value;
        };
    if (log)
        LOGF_INFO("text {} = {}", reinterpret_cast<const char*>(base + id), text());
    if (!OfflineMode())
        return;

    struct Replacement
    {
        const char* id;
        const char* english; // How the original starts.
        const char* text;
    };
    static const Replacement replacements[] = {
        { "TXT_ONLINE_REQUIRED", "You must be signed into an Xbox LIVE",
            "Global Events need EA's online servers, which have been shut down. World Tour and Explore play fully offline." },
        { "TXT_NOT_SIGN_IN_TO_FIRST_PARTY_NETWORK", "You must be signed in to Xbox LIVE",
            "EA's online servers for SSX have been shut down. World Tour and Explore play fully offline." },
        { "TXT_ACTIVITY_FEED_OFFLINE", "Requires connection to Xbox LIVE", "Needs EA's online servers, which are shut down." },
        { "TXT_MAIN_MENU_GLOBAL_EVENTS_DESC", "Compete against the world", "Online only: EA's servers have been shut down." },
    };
    const char* idText = reinterpret_cast<const char*>(base + id);
    for (const Replacement& replacement : replacements)
    {
        if (strcmp(idText, replacement.id) != 0)
            continue;
        if (text().rfind(replacement.english, 0) != 0)
            return;
        uint32_t begin = PPC_LOAD_U32(out), capacity = PPC_LOAD_U32(out + 8);
        uint32_t length = uint32_t(strlen(replacement.text));
        if (begin == 0 || capacity < begin + length + 1)
            return;
        memcpy(base + begin, replacement.text, length + 1);
        PPC_STORE_U32(out + 4, begin + length);
        return;
    }
}

// When SSX's sign-in to the EA servers ends (the state machine at sub_824B1098, at boot and from "Connect to EA
// Server"), sub_824ADB28(self, result) keeps the result at self+28 and raises the front end's fe_signin_success (result
// 0) or fe_signin_failed event. The latter brings up "You must be signed in to Xbox LIVE and the EA Servers in order to
// access the online features of this title" (TXT_NOT_SIGN_IN_TO_FIRST_PARTY_NETWORK, result 2), which came up on every
// boot: there is no Live and the servers are gone. The boot flow waits on one of the two events (with neither it stays
// on "Loading"), so a failure now raises fe_signin_success and keeps the real result. The menus still read the online
// state on their own: "Connect to EA Server" stays, and choosing it still says the servers can't be reached. Saves are
// local content either way (user/content). AR_NO_SIGNIN_BYPASS brings the notice back.
PPC_FUNC_IMPL(__imp__sub_824ADB28);
PPC_FUNC(sub_824ADB28)
{
    static const bool bypass = AR_ENV("AR_NO_SIGNIN_BYPASS") == nullptr;
    uint32_t self = ctx.r3.u32, result = ctx.r4.u32;
    if (bypass && result != 0)
        ctx.r4.u64 = 0;
    __imp__sub_824ADB28(ctx, base);
    if (bypass && result != 0)
        PPC_STORE_U32(self + 28, result);
}
