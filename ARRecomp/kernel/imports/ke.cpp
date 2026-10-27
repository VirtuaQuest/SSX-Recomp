// Threads, synchronization, TLS and time.
#include <stdafx.h>
#include <kernel/imports.h>
#include <cpu/guest_thread.h>

namespace imports
{
    // ---- Threads ----

    uint32_t ExCreateThread(be<uint32_t>* handle, uint32_t stackSize, be<uint32_t>* threadId, uint32_t xapiStartup,
        uint32_t startAddress, uint32_t startContext, uint32_t creationFlags)
    {
        ThreadStartParams params{};
        params.startAddress = startAddress;
        params.startContext = startContext;
        params.xapiStartup = xapiStartup;
        params.stackSize = stackSize;
        params.creationFlags = creationFlags;

        auto thread = CreateGuestThread(params);
        if (!thread)
            return X_STATUS_NO_MEMORY;

        if (handle != nullptr)
            *handle = CreateHandle(thread);
        if (threadId != nullptr)
            *threadId = thread->threadId;

        return X_STATUS_SUCCESS;
    }

    void ExTerminateThread(uint32_t exitCode)
    {
        ExitGuestThread(exitCode);
    }

    uint32_t NtResumeThread(uint32_t handle, be<uint32_t>* suspendCount)
    {
        auto thread = LookupHandle<ThreadObject>(handle, ObjectType::Thread);
        if (!thread)
            return X_STATUS_INVALID_HANDLE;

        uint32_t previous = ResumeGuestThread(thread.get());
        if (suspendCount != nullptr)
            *suspendCount = previous;

        return X_STATUS_SUCCESS;
    }

    uint32_t NtSuspendThread(uint32_t handle, be<uint32_t>* suspendCount)
    {
        auto thread = LookupHandle<ThreadObject>(handle, ObjectType::Thread);
        if (!thread)
            return X_STATUS_INVALID_HANDLE;

        uint32_t previous = SuspendGuestThread(thread.get());
        if (suspendCount != nullptr)
            *suspendCount = previous;

        return X_STATUS_SUCCESS;
    }

    // Guest priorities range from -15 to 15 around 0; map them onto the host priority classes. Low priorities stay
    // close to normal: on the console a low priority thread still runs on its hardware thread whenever others wait,
    // while on the host, threads that poll (the GPU, D3D waiting on queries) keep cores busy, and idle priority
    // threads would barely run. Sofdec's movie decoder lowers itself to -16 and then decodes far below real time.
    static int HostPriority(int32_t priority)
    {
        if (priority >= 15) return THREAD_PRIORITY_TIME_CRITICAL;
        if (priority >= 2) return THREAD_PRIORITY_HIGHEST;
        if (priority >= 1) return THREAD_PRIORITY_ABOVE_NORMAL;
        if (priority <= -1) return THREAD_PRIORITY_BELOW_NORMAL;
        return THREAD_PRIORITY_NORMAL;
    }

    static std::mutex g_priorityMutex;
    static std::unordered_map<uint32_t, int32_t> g_basePriorities;

    int32_t KeQueryBasePriorityThread(uint32_t kthread)
    {
        std::lock_guard lock(g_priorityMutex);
        auto it = g_basePriorities.find(kthread);
        return it != g_basePriorities.end() ? it->second : 0;
    }

    int32_t KeSetBasePriorityThread(uint32_t kthread, int32_t increment)
    {
        int32_t previous;
        {
            std::lock_guard lock(g_priorityMutex);
            previous = g_basePriorities[kthread];
            g_basePriorities[kthread] = increment;
        }

        if (auto thread = FindThreadByKThread(kthread))
            SetThreadPriority(thread->hostThread, HostPriority(increment));

        return previous;
    }

    uint32_t KeSetAffinityThread(uint32_t kthread, uint32_t affinity, be<uint32_t>* previousAffinity)
    {
        // The host scheduler decides where threads run; only remember the requested processor.
        if (previousAffinity != nullptr)
            *previousAffinity = 1u << *GuestPtr<uint8_t>(kthread - PCR_SIZE + 0x10C);

        if (affinity != 0)
            *GuestPtr<uint8_t>(kthread - PCR_SIZE + 0x10C) = uint8_t(std::countr_zero(affinity));

        return X_STATUS_SUCCESS;
    }

    static std::atomic<uint32_t> g_processType{ 1 };

    uint32_t KeGetCurrentProcessType()
    {
        return g_processType;
    }

    void KeSetCurrentProcessType(uint32_t type)
    {
        g_processType = type;
    }

    void KeEnterCriticalRegion()
    {
    }

    void KeLeaveCriticalRegion()
    {
    }

    uint32_t KeDelayExecutionThread(uint32_t processorMode, uint32_t alertable, be<int64_t>* interval)
    {
        if (getenv("AR_LOG_DELAYS") && interval != nullptr)
        {
            static std::mutex mutex;
            static std::map<std::pair<uint32_t, int64_t>, uint32_t> counts;
            std::lock_guard lock(mutex);
            if (counts[{ GetCurrentGuestThreadId(), interval->get() }]++ == 0)
                LOGF_INFO("thread {} delays {} (100 ns units), alertable {}", GetCurrentGuestThreadId(), interval->get(), alertable);
        }

        // Experiment: Unleashed Recompiled ends alertable delays immediately with STATUS_USER_APC.
        if (alertable && getenv("AR_UR_ALERTABLE"))
            return 0x000000C0; // STATUS_USER_APC

        auto deadline = GuestTimeoutToDeadline(interval);
        if (!deadline.has_value() || *deadline <= std::chrono::steady_clock::now())
            std::this_thread::yield();
        else
            std::this_thread::sleep_until(*deadline);

        return X_STATUS_SUCCESS;
    }

    // ---- TLS ----

    static std::mutex g_tlsMutex;
    static std::vector<bool> g_tlsUsed;

    uint32_t KeTlsAlloc()
    {
        std::lock_guard lock(g_tlsMutex);

        g_tlsUsed.resize(g_tlsInfo.slotCount);
        for (uint32_t i = 0; i < g_tlsUsed.size(); i++)
        {
            if (!g_tlsUsed[i])
            {
                g_tlsUsed[i] = true;
                return i;
            }
        }

        LOGF_ERROR("out of TLS slots");
        return 0xFFFFFFFF;
    }

    uint32_t KeTlsFree(uint32_t index)
    {
        std::lock_guard lock(g_tlsMutex);

        if (index >= g_tlsUsed.size())
            return false;

        g_tlsUsed[index] = false;
        return true;
    }

    static be<uint32_t>* TlsSlot(uint32_t index)
    {
        auto thread = GetCurrentThreadObject();
        if (!thread || index >= g_tlsInfo.slotCount)
            return nullptr;

        return GuestPtr<be<uint32_t>>(thread->tlsSlots + index * 4);
    }

    uint32_t KeTlsGetValue(uint32_t index)
    {
        auto* slot = TlsSlot(index);
        return slot != nullptr ? slot->get() : 0;
    }

    uint32_t KeTlsSetValue(uint32_t index, uint32_t value)
    {
        auto* slot = TlsSlot(index);
        if (slot == nullptr)
            return false;

        *slot = value;
        return true;
    }

    // ---- Dispatcher objects in guest memory ----

    int32_t KeSetEvent(XDISPATCHER_HEADER* event, uint32_t increment, uint32_t wait)
    {
        return SetDispatcherEvent(event);
    }

    int32_t KeResetEvent(XDISPATCHER_HEADER* event)
    {
        return ResetDispatcherEvent(event);
    }

    uint32_t KeWaitForSingleObject(XDISPATCHER_HEADER* object, uint32_t waitReason, uint32_t processorMode,
        uint32_t alertable, be<int64_t>* timeout)
    {
        return WaitForDispatcherObjects({ &object, 1 }, false, GuestTimeoutToDeadline(timeout), alertable != 0);
    }

    uint32_t KeWaitForMultipleObjects(uint32_t count, xpointer<XDISPATCHER_HEADER>* objects, uint32_t waitType,
        uint32_t waitReason, uint32_t processorMode, uint32_t alertable, be<int64_t>* timeout, uint32_t waitBlocks)
    {
        std::vector<XDISPATCHER_HEADER*> headers(count);
        for (uint32_t i = 0; i < count; i++)
            headers[i] = objects[i].get();

        // WaitType 0 is WaitAll, 1 is WaitAny.
        return WaitForDispatcherObjects(headers, waitType == 0, GuestTimeoutToDeadline(timeout), alertable != 0);
    }

    // ---- Handle based synchronization objects ----

    static void WarnIfNamed(const XOBJECT_ATTRIBUTES* attributes)
    {
        if (attributes != nullptr && attributes->Name.get() != nullptr)
            LOGF_WARNING("named object {} is not shared by name", ReadAnsiString(attributes->Name.get()));
    }

    uint32_t NtCreateEvent(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, uint32_t eventType, uint32_t initialState)
    {
        WarnIfNamed(attributes);

        // Event type 0 is NotificationEvent (manual reset), 1 is SynchronizationEvent (auto reset).
        auto event = CreateGuestBackedObject<KernelObject>(sizeof(XDISPATCHER_HEADER), ObjectType::Event);
        event->Header()->Type = eventType == 0 ? DISPATCHER_NOTIFICATION_EVENT : DISPATCHER_SYNCHRONIZATION_EVENT;
        event->Header()->Size = sizeof(XDISPATCHER_HEADER) / 4;
        event->Header()->SignalState = initialState ? 1 : 0;

        *handle = CreateHandle(event);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtSetEvent(uint32_t handle, be<int32_t>* previousState)
    {
        auto event = LookupHandle<KernelObject>(handle, ObjectType::Event);
        if (!event)
            return X_STATUS_INVALID_HANDLE;

        int32_t previous = SetDispatcherEvent(event->Header());
        if (previousState != nullptr)
            *previousState = previous;

        return X_STATUS_SUCCESS;
    }

    uint32_t NtClearEvent(uint32_t handle)
    {
        auto event = LookupHandle<KernelObject>(handle, ObjectType::Event);
        if (!event)
            return X_STATUS_INVALID_HANDLE;

        ResetDispatcherEvent(event->Header());
        return X_STATUS_SUCCESS;
    }

    // KSEMAPHORE: header followed by the limit.
    uint32_t NtCreateSemaphore(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, int32_t count, int32_t limit)
    {
        WarnIfNamed(attributes);

        auto semaphore = CreateGuestBackedObject<KernelObject>(sizeof(XKSEMAPHORE), ObjectType::Semaphore);
        auto* guest = reinterpret_cast<XKSEMAPHORE*>(semaphore->Header());
        guest->Header.Type = DISPATCHER_SEMAPHORE;
        guest->Header.Size = sizeof(XKSEMAPHORE) / 4;
        guest->Header.SignalState = uint32_t(count);
        guest->Limit = uint32_t(limit);

        *handle = CreateHandle(semaphore);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtReleaseSemaphore(uint32_t handle, int32_t releaseCount, be<int32_t>* previousCount)
    {
        auto semaphore = LookupHandle<KernelObject>(handle, ObjectType::Semaphore);
        if (!semaphore)
            return X_STATUS_INVALID_HANDLE;

        auto* guest = reinterpret_cast<XKSEMAPHORE*>(semaphore->Header());
        int32_t previous;
        bool released = ReleaseDispatcherSemaphore(&guest->Header, releaseCount, int32_t(guest->Limit.get()), &previous);
        if (previousCount != nullptr)
            *previousCount = previous;

        return released ? X_STATUS_SUCCESS : X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }

    struct TimerObject : KernelObject
    {
        TimerObject() : KernelObject(ObjectType::Timer) {}

        ~TimerObject() override
        {
            CancelDispatcherTimer(Header());
        }
    };

    // KTIMER is 0x28 bytes: header, due time, timer list entry, DPC, period.
    uint32_t NtCreateTimer(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, uint32_t timerType)
    {
        WarnIfNamed(attributes);

        // Timer type 0 is NotificationTimer, 1 is SynchronizationTimer.
        auto timer = CreateGuestBackedObject<TimerObject>(0x28);
        timer->Header()->Type = timerType == 0 ? DISPATCHER_NOTIFICATION_TIMER : DISPATCHER_SYNCHRONIZATION_TIMER;
        timer->Header()->Size = 0x28 / 4;

        *handle = CreateHandle(timer);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtSetTimerEx(uint32_t handle, be<int64_t>* dueTime, uint32_t apcRoutine, uint32_t apcMode,
        uint32_t apcArgument, uint32_t resume, int32_t periodMs, uint32_t reserved)
    {
        auto timer = LookupHandle<KernelObject>(handle, ObjectType::Timer);
        if (!timer)
            return X_STATUS_INVALID_HANDLE;

        if (apcRoutine != 0)
            LOGF_WARNING("timer APC routine 0x{:08X} is not called", apcRoutine);

        SetDispatcherTimer(timer->Header(), dueTime, uint32_t(std::max(periodMs, 0)));
        return X_STATUS_SUCCESS;
    }

    uint32_t NtCancelTimer(uint32_t handle, be<uint32_t>* currentState)
    {
        auto timer = LookupHandle<KernelObject>(handle, ObjectType::Timer);
        if (!timer)
            return X_STATUS_INVALID_HANDLE;

        CancelDispatcherTimer(timer->Header());
        if (currentState != nullptr)
            *currentState = timer->Header()->SignalState.get();

        return X_STATUS_SUCCESS;
    }

    uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout)
    {
        auto object = LookupHandle(handle);
        if (!object)
            return X_STATUS_INVALID_HANDLE;

        if (object->guestObject == 0)
            return X_STATUS_SUCCESS; // Files and other objects without a dispatcher header are always signaled.

        XDISPATCHER_HEADER* header = object->Header();
        return WaitForDispatcherObjects({ &header, 1 }, false, GuestTimeoutToDeadline(timeout), alertable != 0);
    }

    uint32_t NtClose(uint32_t handle)
    {
        return CloseHandle(handle) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
    }

    uint32_t NtDuplicateObject(uint32_t handle, be<uint32_t>* newHandle, uint32_t options)
    {
        auto object = LookupHandle(handle);
        if (!object)
            return X_STATUS_INVALID_HANDLE;

        if (newHandle != nullptr)
            *newHandle = CreateHandle(object);

        // DUPLICATE_CLOSE_SOURCE
        if (options & 1)
            CloseHandle(handle);

        return X_STATUS_SUCCESS;
    }

    // ---- Object manager ----

    // Objects referenced by ObReferenceObjectByHandle stay alive until dereferenced, even if their handles close.
    static std::mutex g_referenceMutex;
    static std::unordered_map<uint32_t, std::pair<std::shared_ptr<KernelObject>, uint32_t>> g_references;

    static void AddReference(const std::shared_ptr<KernelObject>& object)
    {
        std::lock_guard lock(g_referenceMutex);
        auto& entry = g_references[object->guestObject];
        entry.first = object;
        entry.second++;
    }

    uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t objectType, be<uint32_t>* object)
    {
        auto kernelObject = LookupHandle(handle);
        if (!kernelObject)
            return X_STATUS_INVALID_HANDLE;

        if (kernelObject->guestObject == 0)
        {
            LOGF_WARNING("handle 0x{:08X} has no guest object", handle);
            return X_STATUS_OBJECT_TYPE_MISMATCH;
        }

        AddReference(kernelObject);
        if (object != nullptr)
            *object = kernelObject->guestObject;

        return X_STATUS_SUCCESS;
    }

    void ObReferenceObject(uint32_t object)
    {
        std::lock_guard lock(g_referenceMutex);
        auto it = g_references.find(object);
        if (it != g_references.end())
            it->second.second++;
    }

    void ObDereferenceObject(uint32_t object)
    {
        std::shared_ptr<KernelObject> released;
        {
            std::lock_guard lock(g_referenceMutex);
            auto it = g_references.find(object);
            if (it == g_references.end())
                return;

            if (--it->second.second == 0)
            {
                released = std::move(it->second.first);
                g_references.erase(it);
            }
        }
    }

    uint32_t ObLookupThreadByThreadId(uint32_t threadId, be<uint32_t>* object)
    {
        auto thread = FindThreadById(threadId);
        if (!thread)
            return X_STATUS_INVALID_PARAMETER;

        AddReference(thread);
        *object = thread->guestObject;
        return X_STATUS_SUCCESS;
    }

    uint32_t ObOpenObjectByPointer(uint32_t object, be<uint32_t>* handle)
    {
        std::shared_ptr<KernelObject> kernelObject;
        if (auto thread = FindThreadByKThread(object))
        {
            kernelObject = thread;
        }
        else
        {
            std::lock_guard lock(g_referenceMutex);
            auto it = g_references.find(object);
            if (it != g_references.end())
                kernelObject = it->second.first;
        }

        if (!kernelObject)
        {
            LOGF_WARNING("unknown object 0x{:08X}", object);
            return X_STATUS_INVALID_PARAMETER;
        }

        *handle = CreateHandle(kernelObject);
        return X_STATUS_SUCCESS;
    }

    // ---- Critical sections ----

    // RTL_CRITICAL_SECTION: synchronization event header, LockCount (+0x10), RecursionCount (+0x14),
    // OwningThread (+0x18). The fields stay big endian since inlined guest code may read them.
    struct XCriticalSection
    {
        XDISPATCHER_HEADER header;
        be<int32_t> lockCount;
        be<int32_t> recursionCount;
        be<uint32_t> owningThread;
    };

    static_assert(sizeof(XCriticalSection) == 0x1C);

    // Atomically adds to a big endian guest value and returns the new value.
    static int32_t AtomicAddBE(be<int32_t>& value, int32_t addend)
    {
        std::atomic_ref<int32_t> raw(value.value);
        int32_t expected = raw.load();
        while (true)
        {
            int32_t updated = int32_t(ByteSwap(uint32_t(int32_t(ByteSwap(uint32_t(expected))) + addend)));
            if (raw.compare_exchange_weak(expected, updated))
                return int32_t(ByteSwap(uint32_t(updated)));
        }
    }

    static bool CompareExchangeBE(be<int32_t>& value, int32_t expected, int32_t desired)
    {
        std::atomic_ref<int32_t> raw(value.value);
        int32_t rawExpected = int32_t(ByteSwap(uint32_t(expected)));
        return raw.compare_exchange_strong(rawExpected, int32_t(ByteSwap(uint32_t(desired))));
    }

    void RtlInitializeCriticalSection(XCriticalSection* cs)
    {
        memset(cs, 0, sizeof(*cs));
        cs->header.Type = DISPATCHER_SYNCHRONIZATION_EVENT;
        cs->header.Size = sizeof(XDISPATCHER_HEADER) / 4;
        cs->lockCount = -1;
    }

    uint32_t RtlInitializeCriticalSectionAndSpinCount(XCriticalSection* cs, uint32_t spinCount)
    {
        RtlInitializeCriticalSection(cs);
        cs->header.Absolute = uint8_t(std::min<uint32_t>((spinCount + 255) / 256, 255)); // Spin count lives in byte 1.
        return X_STATUS_SUCCESS;
    }

    void RtlEnterCriticalSection(XCriticalSection* cs)
    {
        uint32_t thread = GetCurrentKThread();

        // Spin briefly before blocking, most critical sections are held for a very short time.
        for (int spin = 0; spin < 64; spin++)
        {
            if (CompareExchangeBE(cs->lockCount, -1, 0))
            {
                cs->owningThread = thread;
                cs->recursionCount = 1;
                return;
            }

            if (cs->owningThread == thread)
                break;

            _mm_pause();
        }

        if (AtomicAddBE(cs->lockCount, 1) == 0)
        {
            cs->owningThread = thread;
            cs->recursionCount = 1;
            return;
        }

        if (cs->owningThread == thread)
        {
            cs->recursionCount = cs->recursionCount + 1;
            return;
        }

        XDISPATCHER_HEADER* header = &cs->header;
        WaitForDispatcherObjects({ &header, 1 }, false, std::nullopt, false);
        cs->owningThread = thread;
        cs->recursionCount = 1;
    }

    uint32_t RtlTryEnterCriticalSection(XCriticalSection* cs)
    {
        uint32_t thread = GetCurrentKThread();

        if (CompareExchangeBE(cs->lockCount, -1, 0))
        {
            cs->owningThread = thread;
            cs->recursionCount = 1;
            return true;
        }

        if (cs->owningThread == thread)
        {
            AtomicAddBE(cs->lockCount, 1);
            cs->recursionCount = cs->recursionCount + 1;
            return true;
        }

        return false;
    }

    void RtlLeaveCriticalSection(XCriticalSection* cs)
    {
        int32_t recursion = cs->recursionCount - 1;
        cs->recursionCount = recursion;
        if (recursion != 0)
        {
            AtomicAddBE(cs->lockCount, -1);
            return;
        }

        cs->owningThread = 0;
        if (AtomicAddBE(cs->lockCount, -1) >= 0)
            SetDispatcherEvent(&cs->header); // Wake one waiter.
    }

    // ---- Spin locks ----

    static void AcquireSpinLock(be<uint32_t>* lock)
    {
        uint32_t owner = ByteSwap(GetCurrentKThread() | 1);
        std::atomic_ref<uint32_t> raw(lock->value);
        while (true)
        {
            uint32_t expected = 0;
            if (raw.compare_exchange_weak(expected, owner))
                return;

            _mm_pause();
        }
    }

    static void ReleaseSpinLock(be<uint32_t>* lock)
    {
        std::atomic_ref<uint32_t>(lock->value).store(0);
    }

    uint32_t KfAcquireSpinLock(be<uint32_t>* lock)
    {
        AcquireSpinLock(lock);
        return 0; // Previous IRQL.
    }

    void KfReleaseSpinLock(be<uint32_t>* lock, uint32_t oldIrql)
    {
        ReleaseSpinLock(lock);
    }

    void KeAcquireSpinLockAtRaisedIrql(be<uint32_t>* lock)
    {
        AcquireSpinLock(lock);
    }

    void KeReleaseSpinLockFromRaisedIrql(be<uint32_t>* lock)
    {
        ReleaseSpinLock(lock);
    }

    // ---- Time ----

    static const uint64_t g_hostCounterFrequency = []
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return uint64_t(frequency.QuadPart);
        }();

    uint64_t KeQueryPerformanceFrequency()
    {
        return 50000000; // The Xbox 360 time base runs at 50 MHz.
    }

    void KeQuerySystemTime(be<uint64_t>* time)
    {
        if (time != nullptr)
            *time = uint64_t(GetGuestSystemTime());
    }

    // ---- DPCs ----

    // KDPC: Type (+0x00), list entry (+0x04), DeferredRoutine (+0x0C), DeferredContext (+0x10),
    // SystemArgument1 (+0x14), SystemArgument2 (+0x18).
    struct XKDPC
    {
        be<uint16_t> type;
        uint8_t selectedCpu;
        uint8_t desiredCpu;
        XLIST_ENTRY listEntry;
        be<uint32_t> routine;
        be<uint32_t> context;
        be<uint32_t> argument1;
        be<uint32_t> argument2;
    };

    void KeInitializeDpc(XKDPC* dpc, uint32_t routine, uint32_t context)
    {
        memset(dpc, 0, sizeof(*dpc));
        dpc->type = 19;
        dpc->routine = routine;
        dpc->context = context;
    }

    // DPCs run on a dedicated guest thread, in the order they were queued.
    static std::mutex g_dpcMutex;
    static std::condition_variable g_dpcCondition;
    static std::vector<uint32_t> g_dpcQueue;
    static std::once_flag g_dpcThreadStarted;

    static void RunDpcThread()
    {
        while (true)
        {
            uint32_t address;
            {
                std::unique_lock lock(g_dpcMutex);
                g_dpcCondition.wait(lock, [] { return !g_dpcQueue.empty(); });
                address = g_dpcQueue.front();
                g_dpcQueue.erase(g_dpcQueue.begin());
            }

            auto* dpc = GuestPtr<XKDPC>(address);
            dpc->listEntry.Flink = 0; // No longer queued.
            GuestToHostFunction<void>(dpc->routine.get(), address, dpc->context.get(), dpc->argument1.get(), dpc->argument2.get());
        }
    }

    uint32_t KeInsertQueueDpc(XKDPC* dpc, uint32_t argument1, uint32_t argument2)
    {
        std::call_once(g_dpcThreadStarted, []
            {
                ThreadStartParams params{};
                params.hostEntry = RunDpcThread;
                params.name = "DPC thread";
                CreateGuestThread(params);
            });

        uint32_t address = GuestAddress(dpc);
        std::lock_guard lock(g_dpcMutex);

        // Already queued.
        if (dpc->listEntry.Flink != 0)
            return false;

        dpc->listEntry.Flink = address;
        dpc->argument1 = argument1;
        dpc->argument2 = argument2;
        g_dpcQueue.push_back(address);
        g_dpcCondition.notify_one();
        return true;
    }

    void KiApcNormalRoutineNop()
    {
    }

    // ---- Misc ----

    void KeBugCheck(uint32_t code)
    {
        LOGF_ERROR("KeBugCheck 0x{:08X}", code);
        std::abort();
    }

    void KeBugCheckEx(uint32_t code, uint32_t parameter1, uint32_t parameter2, uint32_t parameter3, uint32_t parameter4)
    {
        LOGF_ERROR("KeBugCheckEx 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})", code, parameter1, parameter2, parameter3, parameter4);
        std::abort();
    }

    // The L2 cache locking is a performance hint.
    uint32_t KeLockL2()
    {
        return 0;
    }

    void KeUnlockL2()
    {
    }
}

AR_IMPORT(ExCreateThread);
AR_IMPORT(ExTerminateThread);
AR_IMPORT(NtResumeThread);
AR_IMPORT(NtSuspendThread);
AR_IMPORT(KeQueryBasePriorityThread);
AR_IMPORT(KeSetBasePriorityThread);
AR_IMPORT(KeSetAffinityThread);
AR_IMPORT(KeGetCurrentProcessType);
AR_IMPORT(KeSetCurrentProcessType);
AR_IMPORT(KeEnterCriticalRegion);
AR_IMPORT(KeLeaveCriticalRegion);
AR_IMPORT(KeDelayExecutionThread);
AR_IMPORT(KeTlsAlloc);
AR_IMPORT(KeTlsFree);
AR_IMPORT(KeTlsGetValue);
AR_IMPORT(KeTlsSetValue);
AR_IMPORT(KeSetEvent);
AR_IMPORT(KeResetEvent);
AR_IMPORT(KeWaitForSingleObject);
AR_IMPORT(KeWaitForMultipleObjects);
AR_IMPORT(NtCreateEvent);
AR_IMPORT(NtSetEvent);
AR_IMPORT(NtClearEvent);
AR_IMPORT(NtCreateSemaphore);
AR_IMPORT(NtReleaseSemaphore);
AR_IMPORT(NtCreateTimer);
AR_IMPORT(NtSetTimerEx);
AR_IMPORT(NtCancelTimer);
AR_IMPORT(NtWaitForSingleObjectEx);
AR_IMPORT(NtClose);
AR_IMPORT(NtDuplicateObject);
AR_IMPORT(ObReferenceObjectByHandle);
AR_IMPORT(ObReferenceObject);
AR_IMPORT(ObDereferenceObject);
AR_IMPORT(ObLookupThreadByThreadId);
AR_IMPORT(ObOpenObjectByPointer);
AR_IMPORT(RtlInitializeCriticalSection);
AR_IMPORT(RtlInitializeCriticalSectionAndSpinCount);
AR_IMPORT(RtlEnterCriticalSection);
AR_IMPORT(RtlTryEnterCriticalSection);
AR_IMPORT(RtlLeaveCriticalSection);
AR_IMPORT(KfAcquireSpinLock);
AR_IMPORT(KfReleaseSpinLock);
AR_IMPORT(KeAcquireSpinLockAtRaisedIrql);
AR_IMPORT(KeReleaseSpinLockFromRaisedIrql);
AR_IMPORT(KeQueryPerformanceFrequency);

uint64_t ArQueryTimeBase()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t ticks = uint64_t(counter.QuadPart);
    uint64_t frequency = imports::g_hostCounterFrequency;
    return ticks / frequency * 50000000 + ticks % frequency * 50000000 / frequency;
}
AR_IMPORT(KeQuerySystemTime);
AR_IMPORT(KeInitializeDpc);
AR_IMPORT(KeInsertQueueDpc);
AR_IMPORT(KiApcNormalRoutineNop);
AR_IMPORT(KeBugCheck);
AR_IMPORT(KeBugCheckEx);
AR_IMPORT(KeLockL2);
AR_IMPORT(KeUnlockL2);
