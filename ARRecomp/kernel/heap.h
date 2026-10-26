#pragma once

#include <o1heap.h>

// Heap for memory the runtime allocates inside guest memory on its own behalf: kernel objects, thread blocks and
// stacks, kernel variables. Based on Unleashed Recompiled's kernel/heap.h (GPL-3.0).
struct Heap
{
    std::mutex mutex;
    O1HeapInstance* heap{};

    void Init();

    void* Alloc(size_t size, size_t alignment = 16);
    void Free(void* ptr);

    // Allocation in guest memory, returned as a guest address. Memory is zeroed.
    uint32_t AllocGuest(size_t size, size_t alignment = 16);
    void FreeGuest(uint32_t address);

    template<typename T, typename... Args>
    T* New(Args&&... args)
    {
        T* obj = (T*)Alloc(sizeof(T), alignof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }
};

extern Heap g_runtimeHeap;
