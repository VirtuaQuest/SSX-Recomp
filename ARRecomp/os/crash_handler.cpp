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
static uint8_t* g_watchPages[3];
static std::set<DWORD64> g_watchSeen;
static thread_local bool t_watchStepping;

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
        ProtectWatch(PAGE_READONLY);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;

    auto* address = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
    bool watched = false;
    for (auto* page : g_watchPages)
        watched |= address >= page && address < page + 4096;
    if (!watched)
        return EXCEPTION_CONTINUE_SEARCH;

    {
        std::lock_guard lock(g_crashMutex);
        if (g_watchSeen.insert(info->ContextRecord->Rip).second && g_watchSeen.size() < 64)
        {
            fmt::print(stderr, "==== watch: write to guest 0x{:08X} on guest thread {}\n", g_memory.MapVirtual(address),
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
    const char* text = getenv("AR_WATCH_PHYSICAL");
    if (!text)
        return;

    uint32_t page = uint32_t(strtoul(text, nullptr, 16)) & ~0xFFFu;
    g_watchPages[0] = g_memory.base + 0xA0000000u + page;
    g_watchPages[1] = g_memory.base + 0xC0000000u + page;
    g_watchPages[2] = g_memory.base + 0xE0000000u + page; // Virtual 0xE0000000 + X is physical X + 0x1000, stored 4 KB up.
    AddVectoredExceptionHandler(1, WatchHandler);
    ProtectWatch(PAGE_READONLY);
}
