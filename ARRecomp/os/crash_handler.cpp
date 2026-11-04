#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <cpu/guest_thread.h>
#include <kernel/memory.h>
#include <dbghelp.h>

// Prints the faulting address, the guest registers and a symbolized host stack on crashes. Recompiled functions
// are named sub_<guest address>, so the host stack doubles as the guest call stack.

static std::mutex g_crashMutex;

static void LoadSymbols()
{
    static std::once_flag once;
    std::call_once(once, []
        {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        });
}

static void PrintStack(HANDLE thread, CONTEXT context, int maxFrames)
{
    HANDLE process = GetCurrentProcess();
    LoadSymbols();

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < maxFrames; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
            SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
        {
            break;
        }

        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;

        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
        {
            // The line in the generated code identifies the guest instruction (each one is preceded by a comment).
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line))
                fmt::print(stderr, "  #{:2} {}+0x{:X} ({}:{})\n", i, symbol->Name, displacement, std::filesystem::path(line.FileName).filename().string(), line.LineNumber);
            else
                fmt::print(stderr, "  #{:2} {}+0x{:X}\n", i, symbol->Name, displacement);
        }
        else
            fmt::print(stderr, "  #{:2} 0x{:X}\n", i, frame.AddrPC.Offset);
    }

    fflush(stderr);
}

// Samples where another thread currently is.
void PrintThreadStack(HANDLE thread, int maxFrames)
{
    if (SuspendThread(thread) == DWORD(-1))
        return;

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context))
        PrintStack(thread, context, maxFrames);

    ResumeThread(thread);
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION && code != 0x80000003 /* breakpoint */)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::lock_guard lock(g_crashMutex);

    auto* record = info->ExceptionRecord;
    fmt::print(stderr, "\n==== Crash: exception 0x{:08X} at {} on guest thread {}\n", code, record->ExceptionAddress, GetCurrentGuestThreadId());

    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        auto address = (uint8_t*)record->ExceptionInformation[1];
        const char* operation = record->ExceptionInformation[0] == 1 ? "write" : "read";
        if (g_memory.IsInMemoryRange(address))
            fmt::print(stderr, "     {} of guest address 0x{:08X}\n", operation, g_memory.MapVirtual(address));
        else
            fmt::print(stderr, "     {} of host address {}\n", operation, (void*)address);
    }

    if (auto* ctx = GetPPCContext())
    {
        fmt::print(stderr, "     guest r1 0x{:08X} r3 0x{:08X} r4 0x{:08X} r5 0x{:08X} lr 0x{:08X}\n",
            ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, uint32_t(ctx->lr));
        // The context does not store the registers in order.
        const PPCRegister* gprs[] = { &ctx->r0, &ctx->r1, &ctx->r2, &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7,
            &ctx->r8, &ctx->r9, &ctx->r10, &ctx->r11, &ctx->r12, &ctx->r13, &ctx->r14, &ctx->r15, &ctx->r16, &ctx->r17,
            &ctx->r18, &ctx->r19, &ctx->r20, &ctx->r21, &ctx->r22, &ctx->r23, &ctx->r24, &ctx->r25, &ctx->r26, &ctx->r27,
            &ctx->r28, &ctx->r29, &ctx->r30, &ctx->r31 };
        for (int row = 0; row < 32; row += 8)
        {
            fmt::print(stderr, "    ");
            for (int i = row; i < row + 8; i++)
                fmt::print(stderr, " r{:<2} {:08X}", i, gprs[i]->u32);
            fmt::print(stderr, "\n");
        }
    }

    PrintStack(GetCurrentThread(), *info->ContextRecord, 48);
    TerminateProcess(GetCurrentProcess(), 0xC0DE0000 | (code & 0xFFFF));
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
    AddVectoredExceptionHandler(1, CrashHandler);
}

// Debugging (AR_WATCH_PHYSICAL=<hex physical address>): logs where writes to the 4 KB page holding a physical
// address come from, once per host instruction. The page is made read only in every physical view; a write faults,
// is logged, and runs with the page writable for one instruction.
static std::vector<uint8_t*> g_watchPages;
static size_t g_watchLimit = 64; // Distinct writers logged (AR_WATCH_LIMIT).
static std::set<DWORD64> g_watchSeen;
static thread_local bool t_watchStepping;
static uint8_t* g_watchExact; // Only writes to these 8 bytes are logged, when set.
static std::vector<uint8_t*> g_watchWords; // Or only writes to these words (WatchGuestWords), each writer once per word.
static std::set<std::pair<DWORD64, uint8_t*>> g_watchWordWriters;
static std::atomic<bool> g_watchStopped; // StopWatchingGuestWords: nothing is logged or protected any more.
static std::atomic<bool> g_watchReads;   // WatchGuestWords also logs reads.

static void ProtectWatch(DWORD protection)
{
    DWORD old;
    for (auto* page : g_watchPages)
        VirtualProtect(page, 4096, protection, &old);
}

static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info)
{
    auto* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watchStepping)
    {
        t_watchStepping = false;
        ProtectWatch(g_watchStopped ? PAGE_READWRITE : g_watchReads ? PAGE_NOACCESS : PAGE_READONLY);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || (record->ExceptionInformation[0] != 1 && !(g_watchReads && record->ExceptionInformation[0] == 0)))
        return EXCEPTION_CONTINUE_SEARCH;

    auto* address = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
    bool watched = false;
    for (auto* page : g_watchPages)
        watched |= address >= page && address < page + 4096;
    if (!watched)
        return EXCEPTION_CONTINUE_SEARCH;

    {
        std::lock_guard lock(g_crashMutex);
        bool exact = g_watchExact == nullptr || (address >= g_watchExact && address < g_watchExact + 8);
        static int exactLogs;
        bool log;
        if (!g_watchWords.empty())
        {
            log = false;
            for (auto* word : g_watchWords)
            {
                // Wider than the word: vector stores of whole blocks report the block's start.
                if (address + 16 > word && address < word + 4)
                    log = g_watchWordWriters.insert({ info->ContextRecord->Rip, word }).second && g_watchWordWriters.size() < 256;
            }
        }
        else
        {
            log = exact && (g_watchExact != nullptr ? exactLogs++ < 64 : g_watchSeen.insert(info->ContextRecord->Rip).second && g_watchSeen.size() < g_watchLimit);
        }
        if (log)
        {
            fmt::print(stderr, "==== watch: {} guest 0x{:08X} on guest thread {}\n", record->ExceptionInformation[0] == 0 ? "read of" : "write to", g_memory.MapVirtual(address),
                GetCurrentGuestThreadId());
            if (auto* ctx = GetPPCContext())
                fmt::print(stderr, "     guest r3 0x{:08X} r4 0x{:08X} r5 0x{:08X} r6 0x{:08X} r31 0x{:08X} lr 0x{:08X}\n",
                    ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, ctx->r31.u32, uint32_t(ctx->lr));
            PrintStack(GetCurrentThread(), *info->ContextRecord, 8);
        }
    }

    ProtectWatch(PAGE_READWRITE);
    info->ContextRecord->EFlags |= 0x100; // Trap after the write.
    t_watchStepping = true;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallMemoryWatch()
{
    if (const char* text = getenv("AR_WATCH_VIRTUAL"))
    {
        // A page outside the physical views: watched in its one view.
        uint32_t address = uint32_t(strtoul(text, nullptr, 16));
        uint32_t page = address & ~0xFFFu;
        g_watchExact = g_memory.base + address;
        g_watchPages = { g_memory.base + page };
        AddVectoredExceptionHandler(1, WatchHandler);
        if (const char* delay = getenv("AR_WATCH_DELAY"))
        {
            std::thread([seconds = atoi(delay)]
                {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    fmt::print(stderr, "==== watch armed\n");
                    ProtectWatch(PAGE_READONLY);
                }).detach();
            return;
        }
        ProtectWatch(PAGE_READONLY);
        return;
    }

    const char* text = getenv("AR_WATCH_PHYSICAL");
    if (!text)
        return;

    // AR_WATCH_PHYSICAL_SIZE: bytes to watch from the address (one page by default); AR_WATCH_LIMIT: distinct writers.
    uint32_t start = uint32_t(strtoul(text, nullptr, 16)) & ~0xFFFu;
    const char* sizeText = getenv("AR_WATCH_PHYSICAL_SIZE");
    uint32_t size = sizeText ? uint32_t(strtoul(sizeText, nullptr, 0)) : 4096;
    if (const char* limit = getenv("AR_WATCH_LIMIT"))
        g_watchLimit = strtoul(limit, nullptr, 0);
    for (uint32_t page = start; page < start + size; page += 4096)
    {
        g_watchPages.push_back(g_memory.base + 0xA0000000u + page);
        g_watchPages.push_back(g_memory.base + 0xC0000000u + page);
        g_watchPages.push_back(g_memory.base + 0xE0000000u + page); // Virtual 0xE0000000 + X is physical X + 0x1000, stored 4 KB up.
    }
    AddVectoredExceptionHandler(1, WatchHandler);

    // AR_WATCH_DELAY: seconds before watching (memory written every frame makes the game crawl while watched).
    if (const char* delay = getenv("AR_WATCH_DELAY"))
    {
        std::thread([seconds = atoi(delay)]
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                fmt::print(stderr, "==== watch armed\n");
                ProtectWatch(PAGE_READONLY);
            }).detach();
        return;
    }
    ProtectWatch(PAGE_READONLY);
}

// Debugging: logs the writers of these guest words (each writer once per word) from now on, with their stacks.
void WatchGuestWords(const std::vector<uint32_t>& addresses, bool reads)
{
    std::lock_guard lock(g_crashMutex);
    g_watchStopped = false;
    g_watchReads = reads;
    g_watchWordWriters.clear();
    g_watchWords.clear();
    std::set<uint8_t*> pages;
    for (uint32_t address : addresses)
    {
        g_watchWords.push_back(g_memory.base + address);
        pages.insert(g_memory.base + (address & ~0xFFFu));
        if (address >= 0xA0000000)
        {
            // Physical heap addresses: the other views of the same page too.
            uint32_t physical = address & 0x1FFFFFFF;
            for (uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
                pages.insert(g_memory.base + ((view + physical) & ~0xFFFu)); // As AR_WATCH_PHYSICAL.
        }
    }
    g_watchPages.assign(pages.begin(), pages.end());
    static bool installed;
    if (!installed)
    {
        installed = true;
        AddVectoredExceptionHandler(1, WatchHandler);
    }
    ProtectWatch(reads ? PAGE_NOACCESS : PAGE_READONLY);
}

// Ends WatchGuestWords, whose page faults otherwise slow the game to a few frames per second.
void StopWatchingGuestWords()
{
    g_watchStopped = true;
    ProtectWatch(PAGE_READWRITE);
    g_watchReads = false;
}

// Prints the calling thread's host stack (recompiled functions are named after their guest addresses).
void PrintCurrentStack(int maxFrames)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    std::lock_guard lock(g_crashMutex);
    PrintStack(GetCurrentThread(), context, maxFrames);
}
