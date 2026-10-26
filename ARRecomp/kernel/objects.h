#pragma once

#include "heap.h"
#include "memory.h"

// NTSTATUS values used by the kernel implementation.
constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_WAIT_0 = 0x00000000;
constexpr uint32_t X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
constexpr uint32_t X_STATUS_ALERTED = 0x00000101;
constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t X_STATUS_PENDING = 0x00000103;
constexpr uint32_t X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr uint32_t X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr uint32_t X_STATUS_OBJECT_NAME_INVALID = 0xC0000033;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
constexpr uint32_t X_STATUS_NOT_SUPPORTED = 0xC00000BB;
constexpr uint32_t X_STATUS_NOT_A_DIRECTORY = 0xC0000103;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;
constexpr uint32_t X_STATUS_INVALID_PAGE_PROTECTION = 0xC0000045;
constexpr uint32_t X_STATUS_CONFLICTING_ADDRESSES = 0xC0000018;

constexpr uint32_t X_INFINITE = 0xFFFFFFFF;
constexpr uint32_t X_CURRENT_PROCESS = 0xFFFFFFFF;
constexpr uint32_t X_CURRENT_THREAD = 0xFFFFFFFE;

// DISPATCHER_HEADER.Type values.
enum DispatcherType : uint8_t
{
    DISPATCHER_NOTIFICATION_EVENT = 0,
    DISPATCHER_SYNCHRONIZATION_EVENT = 1,
    DISPATCHER_MUTANT = 2,
    DISPATCHER_PROCESS = 3,
    DISPATCHER_QUEUE = 4,
    DISPATCHER_SEMAPHORE = 5,
    DISPATCHER_THREAD = 6,
    DISPATCHER_NOTIFICATION_TIMER = 8,
    DISPATCHER_SYNCHRONIZATION_TIMER = 9,
};

// Object types used by ObReferenceObjectByHandle. The values are the guest addresses of the exported type objects.
enum class ObjectType
{
    None,
    Event,
    Semaphore,
    Mutant,
    Timer,
    Thread,
    File,
    SymbolicLink,
    Other,
};

// A kernel object referenced by handles. Waitable objects own a dispatcher header in guest memory.
struct KernelObject
{
    ObjectType type{};
    uint32_t guestObject{}; // Guest address of the object (dispatcher header first for waitable objects).
    bool ownsGuestObject{};

    KernelObject(ObjectType type) : type(type) {}
    virtual ~KernelObject();

    XDISPATCHER_HEADER* Header() const { return g_memory.Translate<XDISPATCHER_HEADER>(guestObject); }
};

// Handle table. Handles reference objects shared between duplicates.
uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
std::shared_ptr<KernelObject> LookupHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);

template<typename T>
std::shared_ptr<T> LookupHandle(uint32_t handle, ObjectType type)
{
    auto object = LookupHandle(handle);
    if (!object || object->type != type)
        return nullptr;

    return std::static_pointer_cast<T>(object);
}

// Creates an object with a zeroed guest block of the given size, whose first bytes are a dispatcher header.
template<typename T, typename... Args>
std::shared_ptr<T> CreateGuestBackedObject(uint32_t guestSize, Args&&... args)
{
    auto object = std::make_shared<T>(std::forward<Args>(args)...);
    object->guestObject = g_runtimeHeap.AllocGuest(guestSize);
    object->ownsGuestObject = true;
    return object;
}

// ---- Dispatcher ----

// Converts a guest timeout (100ns units, negative relative, positive absolute since 1601) to a host deadline.
// nullptr means wait forever.
std::optional<std::chrono::steady_clock::time_point> GuestTimeoutToDeadline(const be<int64_t>* timeout);
std::optional<std::chrono::steady_clock::time_point> MillisecondsToDeadline(uint32_t milliseconds);

// Waits for one (or all, when waitAll is set) of the dispatcher objects. Returns STATUS_WAIT_0 + index, or
// STATUS_TIMEOUT. The calling thread's guest KTHREAD address is used for mutant ownership.
uint32_t WaitForDispatcherObjects(std::span<XDISPATCHER_HEADER* const> objects, bool waitAll,
    std::optional<std::chrono::steady_clock::time_point> deadline, bool alertable);

// Signals an event; returns the previous signal state.
int32_t SetDispatcherEvent(XDISPATCHER_HEADER* event);
int32_t ResetDispatcherEvent(XDISPATCHER_HEADER* event);
int32_t PulseDispatcherEvent(XDISPATCHER_HEADER* event);

// Releases a semaphore. Returns false when the limit would be exceeded.
bool ReleaseDispatcherSemaphore(XDISPATCHER_HEADER* semaphore, int32_t count, int32_t limit, int32_t* previousCount);

// Sets the signal state of any dispatcher object and wakes waiters, e.g. for threads finishing.
void SignalDispatcherObject(XDISPATCHER_HEADER* object, int32_t signalState);

// Timers: due time is a guest timeout (see GuestTimeoutToDeadline), period in milliseconds (0 for one shot).
void SetDispatcherTimer(XDISPATCHER_HEADER* timer, const be<int64_t>* dueTime, uint32_t periodMs);
bool CancelDispatcherTimer(XDISPATCHER_HEADER* timer);

// Wakes threads waiting on objects whose state was changed directly in guest memory.
void NotifyDispatcher();

// Current time as a guest FILETIME (100ns units since 1601).
int64_t GetGuestSystemTime();
