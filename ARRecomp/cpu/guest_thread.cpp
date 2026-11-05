#include <stdafx.h>
#include "guest_thread.h"
#include "ppc_context.h"
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <dbghelp.h>

void PrintThreadStack(HANDLE thread, int maxFrames);

TlsInfo g_tlsInfo;
uint32_t g_defaultStackSize = 0x40000;

// Host stack for guest threads. Recompiled guest calls nest on the host stack, so it has to be generous.
constexpr size_t HOST_STACK_SIZE = 64 * 1024 * 1024;

static thread_local std::shared_ptr<ThreadObject> t_currentThread;

static std::atomic<uint32_t> g_nextThreadId{ 1 };
static std::mutex g_threadsMutex;
static std::unordered_map<uint32_t, std::weak_ptr<ThreadObject>> g_threads;

// Thrown by ExitGuestThread to unwind out of guest code back to the thread entry.
struct GuestThreadExit
{
    uint32_t exitCode;
};

ThreadObject::~ThreadObject()
{
    {
        std::lock_guard lock(g_threadsMutex);
        g_threads.erase(threadId);
    }

#ifdef _WIN32
    if (hostThread != nullptr)
        ::CloseHandle(hostThread);
#endif

    g_runtimeHeap.FreeGuest(pcr);
    g_runtimeHeap.FreeGuest(stackLimit);
}

uint32_t* ThreadObject::LastErrorPtr() const
{
    return reinterpret_cast<uint32_t*>(g_memory.Translate(guestObject + 0x160));
}

static void WriteGuest32(uint32_t address, uint32_t value)
{
    *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(address)) = value;
}

#ifdef _WIN32
static DWORD WINAPI HostThreadEntry(void* parameter)
{
    // The creator handed over a reference in self; take it so the object lives as long as this thread runs.
    auto* raw = static_cast<ThreadObject*>(parameter);
    std::shared_ptr<ThreadObject> thread = raw->self;
    t_currentThread = thread;

    PPCContext ctx{};
    ctx.r1.u64 = thread->stackBase - 0x80;
    ctx.r13.u64 = thread->pcr;
    ctx.fpscr.loadFromHost();
    SetPPCContext(ctx);

    uint32_t exitCode;
    try
    {
        const auto& params = thread->params;
        if (params.hostEntry)
        {
            params.hostEntry();
            ctx.r3.u64 = 0;
        }
        else if (params.xapiStartup != 0)
        {
            ctx.r3.u64 = params.startAddress;
            ctx.r4.u64 = params.startContext;
            g_memory.FindFunction(params.xapiStartup)(ctx, g_memory.base);
        }
        else
        {
            ctx.r3.u64 = params.startContext;
            g_memory.FindFunction(params.startAddress)(ctx, g_memory.base);
        }

        exitCode = ctx.r3.u32;
    }
    catch (const GuestThreadExit& exit)
    {
        exitCode = exit.exitCode;
    }

    LOGF_INFO("thread {} exited with code 0x{:X}", thread->threadId, exitCode);

    thread->exitCode = exitCode;
    thread->finished = true;
    SignalDispatcherObject(thread->Header(), 1);

    thread->self.reset();
    t_currentThread.reset();
    return exitCode;
}
#endif

std::shared_ptr<ThreadObject> CreateGuestThread(const ThreadStartParams& params)
{
    auto thread = std::make_shared<ThreadObject>();
    thread->params = params;
    thread->threadId = g_nextThreadId++;

    // PCR, KTHREAD and TLS share one block.
    uint32_t tlsStaticSize = (g_tlsInfo.dataSize + 15) & ~15u;
    uint32_t blockSize = PCR_SIZE + KTHREAD_SIZE + tlsStaticSize + g_tlsInfo.slotCount * 4;
    thread->pcr = g_runtimeHeap.AllocGuest(blockSize, 0x1000);
    thread->guestObject = thread->pcr + PCR_SIZE;
    thread->tls = thread->guestObject + KTHREAD_SIZE;
    thread->tlsSlots = thread->tls + tlsStaticSize;

    uint32_t stackSize = (params.stackSize != 0 ? params.stackSize : g_defaultStackSize);
    stackSize = (std::max<uint32_t>(stackSize, 0x10000) + 0xFFF) & ~0xFFFu;
    thread->stackLimit = g_runtimeHeap.AllocGuest(stackSize, 0x1000);
    thread->stackBase = thread->stackLimit + stackSize;

    if (g_tlsInfo.rawDataSize != 0)
        memcpy(g_memory.Translate(thread->tls), g_memory.Translate(g_tlsInfo.rawDataAddress), g_tlsInfo.rawDataSize);

    uint32_t pcr = thread->pcr;
    uint32_t kthread = thread->guestObject;
    WriteGuest32(pcr + 0x000, thread->tls);
    WriteGuest32(pcr + 0x030, pcr);
    WriteGuest32(pcr + 0x070, thread->stackBase);
    WriteGuest32(pcr + 0x074, thread->stackLimit);
    WriteGuest32(pcr + 0x100, kthread);

    // Spread threads over the six hardware threads according to the requested affinity mask.
    uint8_t affinity = uint8_t(params.creationFlags >> 24);
    *g_memory.Translate<uint8_t>(pcr + 0x10C) = affinity != 0 ? uint8_t(std::countr_zero(affinity)) : 0;

    auto* header = thread->Header();
    header->Type = DISPATCHER_THREAD;
    header->SignalState = 0;
    WriteGuest32(kthread + 0x05C, thread->stackBase);
    WriteGuest32(kthread + 0x060, thread->stackLimit);
    WriteGuest32(kthread + 0x068, thread->tls);
    WriteGuest32(kthread + 0x14C, thread->threadId);
    WriteGuest32(kthread + 0x150, params.startAddress);

    {
        std::lock_guard lock(g_threadsMutex);
        g_threads.emplace(thread->threadId, thread);
    }

    bool suspended = (params.creationFlags & 1) != 0;
    thread->suspendCount = suspended ? 1 : 0;
    thread->self = thread;

#ifdef _WIN32
    thread->hostThread = ::CreateThread(nullptr, HOST_STACK_SIZE, HostThreadEntry, thread.get(),
        STACK_SIZE_PARAM_IS_A_RESERVATION | (suspended ? CREATE_SUSPENDED : 0), nullptr);
    if (thread->hostThread == nullptr)
    {
        LOGF_ERROR("failed to create host thread");
        thread->self.reset();
        return nullptr;
    }

    std::string name = params.name.empty() ? fmt::format("Guest thread {}", thread->threadId) : params.name;
    SetThreadDescription(thread->hostThread, std::filesystem::path(name).wstring().c_str());
#endif

    LOGF_INFO("thread {} created: start 0x{:08X} context 0x{:08X} xapi 0x{:08X} stack 0x{:X} flags 0x{:X}",
        thread->threadId, params.startAddress, params.startContext, params.xapiStartup, stackSize, params.creationFlags);

    return thread;
}

uint32_t SuspendGuestThread(ThreadObject* thread)
{
#ifdef _WIN32
    DWORD previous = ::SuspendThread(thread->hostThread);
    thread->suspendCount++;
    return previous;
#endif
}

uint32_t ResumeGuestThread(ThreadObject* thread)
{
#ifdef _WIN32
    DWORD previous = ::ResumeThread(thread->hostThread);
    if (previous > 0)
        thread->suspendCount--;
    return previous;
#endif
}

void ExitGuestThread(uint32_t exitCode)
{
    throw GuestThreadExit{ exitCode };
}

std::shared_ptr<ThreadObject> GetCurrentThreadObject()
{
    return t_currentThread;
}

uint32_t GetCurrentKThread()
{
    return t_currentThread ? t_currentThread->guestObject : 0;
}

uint32_t GetCurrentGuestThreadId()
{
    return t_currentThread ? t_currentThread->threadId : 0;
}

std::shared_ptr<ThreadObject> FindThreadById(uint32_t threadId)
{
    std::lock_guard lock(g_threadsMutex);

    auto it = g_threads.find(threadId);
    return it != g_threads.end() ? it->second.lock() : nullptr;
}

void RecordImportCall(const char* name, uint32_t caller)
{
    if (auto* thread = t_currentThread.get())
    {
        thread->lastImport.store(name, std::memory_order_relaxed);
        thread->lastImportCaller.store(caller, std::memory_order_relaxed);
    }
}

void RecordWait(uint32_t object)
{
    if (auto* thread = t_currentThread.get())
        thread->waitingOn.store(object, std::memory_order_relaxed);
}

void DumpGuestThreads()
{
    std::vector<std::shared_ptr<ThreadObject>> threads;
    {
        std::lock_guard lock(g_threadsMutex);
        for (const auto& [id, weak] : g_threads)
        {
            if (auto thread = weak.lock())
                threads.push_back(thread);
        }
    }

    std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a->threadId < b->threadId; });
    for (const auto& thread : threads)
    {
        const char* last = thread->lastImport.load();
        uint32_t waiting = thread->waitingOn.load();
        FILETIME creation, exit, kernel, user;
        double cpuSeconds = 0.0;
        if (!thread->finished && GetThreadTimes(thread->hostThread, &creation, &exit, &kernel, &user))
            cpuSeconds = double((uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime) + (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime)) / 1e7;

        LOGF_INFO("thread {:3} start 0x{:08X}{}: {:.1f} s CPU, last import {} from 0x{:08X}{}", thread->threadId, thread->params.startAddress,
            thread->finished ? " (finished)" : "", cpuSeconds, last != nullptr ? last : "-", thread->lastImportCaller.load(),
            waiting != 0 ? fmt::format(", waiting on 0x{:08X}", waiting) : "");

        // Threads that are running guest code: show where. AR_DUMP_THREADS_FILE: all of them, as a stopped game is usually waiting.
        if (!thread->finished && (waiting == 0 || AR_ENV("AR_DUMP_THREADS_FILE")))
            PrintThreadStack(thread->hostThread, 26);
    }
}

std::shared_ptr<ThreadObject> FindThreadByKThread(uint32_t kthread)
{
    std::lock_guard lock(g_threadsMutex);

    for (const auto& [id, weak] : g_threads)
    {
        auto thread = weak.lock();
        if (thread && thread->guestObject == kthread)
            return thread;
    }

    return nullptr;
}

// Diagnostics (AR_THREAD_PROFILE): samples where each guest thread runs and reports the busy ones every two seconds.
// Each sample suspends the thread and unwinds to the first frame in the executable, so time in imports and system
// code is attributed to the recompiled function that called them.
void StartThreadProfiler()
{
    if (!AR_ENV("AR_THREAD_PROFILE"))
        return;

    std::thread([]
        {
            SetThreadDescription(GetCurrentThread(), L"Thread profiler");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            HMODULE exe = GetModuleHandleW(nullptr);
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
            uint64_t exeStart = uint64_t(exe), exeEnd = exeStart + nt->OptionalHeader.SizeOfImage;

            struct Samples
            {
                std::unordered_map<uint64_t, uint32_t> callers; // First return address in the executable.
                std::unordered_map<const char*, uint32_t> imports;
                uint32_t total = 0;
                uint64_t lastCpu = 0;
            };
            std::unordered_map<uint32_t, Samples> samples;
            auto lastReport = std::chrono::steady_clock::now();

            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);

            while (true)
            {
                Sleep(1);
                std::vector<std::shared_ptr<ThreadObject>> threads;
                {
                    std::lock_guard lock(g_threadsMutex);
                    for (const auto& [id, weak] : g_threads)
                    {
                        if (auto thread = weak.lock(); thread && !thread->finished)
                            threads.push_back(thread);
                    }
                }

                for (const auto& thread : threads)
                {
                    if (SuspendThread(thread->hostThread) == DWORD(-1))
                        continue;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    uint64_t caller = 0;
                    if (GetThreadContext(thread->hostThread, &context))
                    {
                        // Unwinding only reads the suspended thread's stack and the unwind tables (no allocation).
                        for (int frame = 0; frame < 32 && context.Rip != 0; frame++)
                        {
                            if (context.Rip >= exeStart && context.Rip < exeEnd)
                            {
                                caller = context.Rip;
                                break;
                            }
                            DWORD64 imageBase;
                            auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                            if (function == nullptr)
                            {
                                context.Rip = *reinterpret_cast<const uint64_t*>(context.Rsp);
                                context.Rsp += 8;
                                continue;
                            }
                            void* handlerData;
                            DWORD64 establisher;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
                        }
                    }
                    ResumeThread(thread->hostThread);

                    auto& entry = samples[thread->threadId];
                    entry.callers[caller]++;
                    entry.imports[thread->lastImport.load()]++;
                    entry.total++;
                }

                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - lastReport).count();
                if (elapsed < 2.0)
                    continue;
                lastReport = now;

                for (const auto& thread : threads)
                {
                    auto& entry = samples[thread->threadId];
                    FILETIME creation, exit, kernel, user;
                    uint64_t cpu = 0;
                    if (GetThreadTimes(thread->hostThread, &creation, &exit, &kernel, &user))
                        cpu = (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime) + (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime);
                    double percent = entry.lastCpu != 0 ? double(cpu - entry.lastCpu) / 1e7 / elapsed * 100.0 : 0.0;
                    entry.lastCpu = cpu;

                    if (percent >= 10.0 && entry.total != 0)
                    {
                        std::vector<std::pair<uint32_t, std::string>> top;
                        std::unordered_map<std::string, uint32_t> byFunction;
                        for (const auto& [address, count] : entry.callers)
                        {
                            std::string name = "?";
                            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
                            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                            symbol->MaxNameLen = 255;
                            DWORD64 displacement;
                            if (address != 0 && SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
                                name = symbol->Name;
                            byFunction[name] += count;
                        }
                        for (const auto& [name, count] : byFunction)
                            top.emplace_back(count, name);
                        std::sort(top.begin(), top.end(), std::greater<>());
                        std::string summary;
                        for (size_t i = 0; i < std::min<size_t>(top.size(), 4); i++)
                            summary += fmt::format(" {} {}%", top[i].second, top[i].first * 100 / entry.total);

                        const char* import = nullptr;
                        uint32_t importCount = 0;
                        for (const auto& [name, count] : entry.imports)
                        {
                            if (count > importCount)
                            {
                                import = name;
                                importCount = count;
                            }
                        }
                        LOGF_INFO("thread profile: {:3} start 0x{:08X} {:.0f}% CPU, last import {} ({}%), in{}", thread->threadId,
                            thread->params.startAddress, percent, import ? import : "-", importCount * 100 / entry.total, summary);
                    }
                    entry.callers.clear();
                    entry.imports.clear();
                    entry.total = 0;
                }
            }
        }).detach();
}
