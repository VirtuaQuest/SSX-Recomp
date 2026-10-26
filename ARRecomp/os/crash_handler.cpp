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
            fmt::print(stderr, "  #{:2} {}+0x{:X}\n", i, symbol->Name, displacement);
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
        fmt::print(stderr, "     guest r26 0x{:08X} r27 0x{:08X} r28 0x{:08X} r29 0x{:08X} r30 0x{:08X} r31 0x{:08X}\n",
            ctx->r26.u32, ctx->r27.u32, ctx->r28.u32, ctx->r29.u32, ctx->r30.u32, ctx->r31.u32);
    }

    PrintStack(GetCurrentThread(), *info->ContextRecord, 48);
    TerminateProcess(GetCurrentProcess(), 0xC0DE0000 | (code & 0xFFFF));
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
    AddVectoredExceptionHandler(1, CrashHandler);
}
