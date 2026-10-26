#pragma once

#include <kernel/objects.h>

// Guest thread block layout, following what Xbox 360 code expects to find through r13 (the PCR):
//   PCR + 0x000  pointer to the thread's TLS (static TLS data, followed by the dynamic KeTlsAlloc slots)
//   PCR + 0x030  pointer to the PCR itself
//   PCR + 0x070  stack base, PCR + 0x074 stack limit
//   PCR + 0x100  pointer to the current KTHREAD
//   PCR + 0x10C  current processor number
// KTHREAD:
//   + 0x000  dispatcher header (type 6, signaled when the thread exits)
//   + 0x05C  stack base, + 0x060 stack limit, + 0x068 TLS address
//   + 0x14C  thread id, + 0x150 start address, + 0x160 Win32 last error
constexpr uint32_t PCR_SIZE = 0x400;
constexpr uint32_t KTHREAD_SIZE = 0x400;

struct ThreadStartParams
{
    uint32_t startAddress{};
    uint32_t startContext{};
    uint32_t xapiStartup{}; // When set, the thread runs xapiStartup(startAddress, startContext).
    uint32_t stackSize{};
    uint32_t creationFlags{};
    std::function<void()> hostEntry; // Runs host code with a guest context instead of a guest function.
    std::string name;
};

struct ThreadObject : KernelObject
{
    ThreadStartParams params;
    uint32_t threadId{};
    uint32_t pcr{};       // Guest address of the PCR; the KTHREAD follows it and is the object's guest address.
    uint32_t tls{};       // Guest address of the static TLS data.
    uint32_t tlsSlots{};  // Guest address of the first dynamic TLS slot.
    uint32_t stackBase{}; // Highest address of the stack.
    uint32_t stackLimit{};
    uint32_t exitCode{};
    std::atomic<bool> finished{};
    std::atomic<int32_t> suspendCount{};
    std::shared_ptr<ThreadObject> self; // Keeps the object alive while the thread runs.

    // Diagnostics: the last import called and what the thread is waiting on.
    std::atomic<const char*> lastImport{};
    std::atomic<uint32_t> lastImportCaller{};
    std::atomic<uint32_t> waitingOn{};

#ifdef _WIN32
    HANDLE hostThread{};
#endif

    ThreadObject() : KernelObject(ObjectType::Thread) {}
    ~ThreadObject() override;

    uint32_t* LastErrorPtr() const;
};

// TLS layout from the executable's TLS header, set once at startup.
struct TlsInfo
{
    uint32_t slotCount = 64;
    uint32_t rawDataAddress{};
    uint32_t dataSize{};
    uint32_t rawDataSize{};
};

extern TlsInfo g_tlsInfo;
extern uint32_t g_defaultStackSize;

// Creates and starts a guest thread. Threads created with the suspended flag (1) wait for ResumeGuestThread.
std::shared_ptr<ThreadObject> CreateGuestThread(const ThreadStartParams& params);
uint32_t SuspendGuestThread(ThreadObject* thread);
uint32_t ResumeGuestThread(ThreadObject* thread);

// Ends the current guest thread with the given exit code. Does not return.
[[noreturn]] void ExitGuestThread(uint32_t exitCode);

std::shared_ptr<ThreadObject> GetCurrentThreadObject();
uint32_t GetCurrentKThread();
uint32_t GetCurrentGuestThreadId();
std::shared_ptr<ThreadObject> FindThreadById(uint32_t threadId);
std::shared_ptr<ThreadObject> FindThreadByKThread(uint32_t kthread);

// Records an import call by the current thread, for DumpGuestThreads.
void RecordImportCall(const char* name, uint32_t caller);
void RecordWait(uint32_t object);

// Prints every guest thread's last import and the object it is waiting on.
void DumpGuestThreads();
