#include <stdafx.h>
#include "objects.h"
#include <cpu/guest_thread.h>

// ---- Handle table ----

KernelObject::~KernelObject()
{
    if (ownsGuestObject)
        g_runtimeHeap.FreeGuest(guestObject);
}

static std::mutex g_handleMutex;
static std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
static uint32_t g_nextHandle = 0xF8000004;

uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
{
    std::lock_guard lock(g_handleMutex);

    uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    g_handles.emplace(handle, std::move(object));
    return handle;
}

std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
{
    if (handle == X_CURRENT_THREAD)
        return GetCurrentThreadObject();

    std::lock_guard lock(g_handleMutex);

    auto it = g_handles.find(handle);
    return it != g_handles.end() ? it->second : nullptr;
}

bool CloseHandle(uint32_t handle)
{
    std::shared_ptr<KernelObject> object;
    {
        std::lock_guard lock(g_handleMutex);

        auto it = g_handles.find(handle);
        if (it == g_handles.end())
            return false;

        // Destroy the object outside the lock, destructors may close other handles.
        object = std::move(it->second);
        g_handles.erase(it);
    }

    return true;
}

// ---- Time ----

// Offset between the guest epoch (1601) and the Unix epoch in 100ns units.
constexpr int64_t FILETIME_UNIX_EPOCH = 116444736000000000ll;

int64_t GetGuestSystemTime()
{
    auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count() / 100 + FILETIME_UNIX_EPOCH;
}

std::optional<std::chrono::steady_clock::time_point> GuestTimeoutToDeadline(const be<int64_t>* timeout)
{
    if (timeout == nullptr)
        return std::nullopt;

    int64_t value = timeout->get();
    int64_t relative = value < 0 ? -value : std::max<int64_t>(0, value - GetGuestSystemTime());
    return std::chrono::steady_clock::now() + std::chrono::nanoseconds(relative * 100);
}

std::optional<std::chrono::steady_clock::time_point> MillisecondsToDeadline(uint32_t milliseconds)
{
    if (milliseconds == X_INFINITE)
        return std::nullopt;

    return std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
}

// ---- Dispatcher ----

// All dispatcher state changes and waits are serialized by one lock, and waiters are woken through one condition
// variable. This is simple and correct; it can be refined if contention shows up.
static std::mutex g_dispatcherMutex;
static std::condition_variable g_dispatcherCondition;

struct TimerState
{
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds period{};
    bool active{};
};

static std::unordered_map<XDISPATCHER_HEADER*, TimerState> g_timers;

// KMUTANT: header, MutantListEntry (+0x10), OwnerThread (+0x18), Abandoned (+0x1C).
static be<uint32_t>& MutantOwner(XDISPATCHER_HEADER* header)
{
    return *reinterpret_cast<be<uint32_t>*>(reinterpret_cast<uint8_t*>(header) + 0x18);
}

static int32_t SignalState(XDISPATCHER_HEADER* header)
{
    return int32_t(header->SignalState.get());
}

static void SetSignalState(XDISPATCHER_HEADER* header, int32_t state)
{
    header->SignalState = uint32_t(state);
}

static void UpdateTimer(XDISPATCHER_HEADER* header, std::chrono::steady_clock::time_point now)
{
    auto it = g_timers.find(header);
    if (it == g_timers.end() || !it->second.active || now < it->second.due)
        return;

    SetSignalState(header, 1);

    if (it->second.period.count() != 0)
    {
        while (it->second.due <= now)
            it->second.due += it->second.period;
    }
    else
    {
        it->second.active = false;
    }
}

static bool IsSignaled(XDISPATCHER_HEADER* header, uint32_t thread, std::chrono::steady_clock::time_point now)
{
    switch (header->Type)
    {
    case DISPATCHER_NOTIFICATION_TIMER:
    case DISPATCHER_SYNCHRONIZATION_TIMER:
        UpdateTimer(header, now);
        return SignalState(header) > 0;

    case DISPATCHER_MUTANT:
        return SignalState(header) > 0 || MutantOwner(header) == thread;

    default:
        return SignalState(header) > 0;
    }
}

static void Acquire(XDISPATCHER_HEADER* header, uint32_t thread)
{
    switch (header->Type)
    {
    case DISPATCHER_SYNCHRONIZATION_EVENT:
    case DISPATCHER_SYNCHRONIZATION_TIMER:
        SetSignalState(header, 0);
        break;

    case DISPATCHER_SEMAPHORE:
        SetSignalState(header, SignalState(header) - 1);
        break;

    case DISPATCHER_MUTANT:
        SetSignalState(header, SignalState(header) - 1);
        MutantOwner(header) = thread;
        break;
    }
}

uint32_t WaitForDispatcherObjects(std::span<XDISPATCHER_HEADER* const> objects, bool waitAll,
    std::optional<std::chrono::steady_clock::time_point> deadline, bool alertable)
{
    const uint32_t thread = GetCurrentKThread();

    std::unique_lock lock(g_dispatcherMutex);

    while (true)
    {
        auto now = std::chrono::steady_clock::now();

        if (waitAll)
        {
            bool all = std::all_of(objects.begin(), objects.end(), [&](auto* header) { return IsSignaled(header, thread, now); });
            if (all)
            {
                for (auto* header : objects)
                    Acquire(header, thread);

                return X_STATUS_WAIT_0;
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (IsSignaled(objects[i], thread, now))
                {
                    Acquire(objects[i], thread);
                    return X_STATUS_WAIT_0 + uint32_t(i);
                }
            }
        }

        if (deadline.has_value() && now >= *deadline)
            return X_STATUS_TIMEOUT;

        // Wake up for the earliest timer that could satisfy the wait.
        auto wake = deadline;
        for (auto* header : objects)
        {
            auto it = g_timers.find(header);
            if (it != g_timers.end() && it->second.active && (!wake.has_value() || it->second.due < *wake))
                wake = it->second.due;
        }

        if (wake.has_value())
            g_dispatcherCondition.wait_until(lock, *wake);
        else
            g_dispatcherCondition.wait(lock);
    }
}

int32_t SetDispatcherEvent(XDISPATCHER_HEADER* event)
{
    std::lock_guard lock(g_dispatcherMutex);

    int32_t previous = SignalState(event);
    SetSignalState(event, 1);
    g_dispatcherCondition.notify_all();
    return previous;
}

int32_t ResetDispatcherEvent(XDISPATCHER_HEADER* event)
{
    std::lock_guard lock(g_dispatcherMutex);

    int32_t previous = SignalState(event);
    SetSignalState(event, 0);
    return previous;
}

int32_t PulseDispatcherEvent(XDISPATCHER_HEADER* event)
{
    int32_t previous;
    {
        std::lock_guard lock(g_dispatcherMutex);
        previous = SignalState(event);
        SetSignalState(event, 1);
        g_dispatcherCondition.notify_all();
    }

    // Give waiters a chance to observe the signal before it is reset.
    std::this_thread::yield();

    std::lock_guard lock(g_dispatcherMutex);
    SetSignalState(event, 0);
    return previous;
}

bool ReleaseDispatcherSemaphore(XDISPATCHER_HEADER* semaphore, int32_t count, int32_t limit, int32_t* previousCount)
{
    std::lock_guard lock(g_dispatcherMutex);

    int32_t previous = SignalState(semaphore);
    if (previousCount != nullptr)
        *previousCount = previous;

    if (count < 0 || int64_t(previous) + count > limit)
        return false;

    SetSignalState(semaphore, previous + count);
    g_dispatcherCondition.notify_all();
    return true;
}

void SignalDispatcherObject(XDISPATCHER_HEADER* object, int32_t signalState)
{
    std::lock_guard lock(g_dispatcherMutex);

    SetSignalState(object, signalState);
    g_dispatcherCondition.notify_all();
}

void SetDispatcherTimer(XDISPATCHER_HEADER* timer, const be<int64_t>* dueTime, uint32_t periodMs)
{
    auto due = GuestTimeoutToDeadline(dueTime).value_or(std::chrono::steady_clock::now());

    std::lock_guard lock(g_dispatcherMutex);

    SetSignalState(timer, 0);
    g_timers[timer] = TimerState{ due, std::chrono::milliseconds(periodMs), true };
    g_dispatcherCondition.notify_all();
}

bool CancelDispatcherTimer(XDISPATCHER_HEADER* timer)
{
    std::lock_guard lock(g_dispatcherMutex);

    auto it = g_timers.find(timer);
    if (it == g_timers.end())
        return false;

    bool wasActive = it->second.active;
    g_timers.erase(it);
    return wasActive;
}

void NotifyDispatcher()
{
    std::lock_guard lock(g_dispatcherMutex);
    g_dispatcherCondition.notify_all();
}
