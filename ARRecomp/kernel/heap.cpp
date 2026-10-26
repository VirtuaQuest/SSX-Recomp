#include <stdafx.h>
#include "heap.h"
#include "memory.h"

Heap g_runtimeHeap;

void Heap::Init()
{
    heap = o1heapInit(g_memory.Translate(GUEST_RUNTIME_HEAP_BEGIN), GUEST_RUNTIME_HEAP_END - GUEST_RUNTIME_HEAP_BEGIN);
    if (heap == nullptr)
        LOGF_ERROR("failed to create the runtime heap at {}", g_memory.Translate(GUEST_RUNTIME_HEAP_BEGIN));
}

void* Heap::Alloc(size_t size, size_t alignment)
{
    alignment = std::max<size_t>(alignment, 16);

    void* ptr;
    {
        std::lock_guard lock(mutex);
        ptr = o1heapAllocate(heap, size + alignment);
    }

    if (ptr == nullptr)
    {
        LOGF_ERROR("out of runtime heap memory allocating {} bytes", size);
        return nullptr;
    }

    // Keep the original pointer in front of the aligned block for Free.
    size_t aligned = ((size_t)ptr + alignment) & ~(alignment - 1);
    *((void**)aligned - 1) = ptr;
    memset((void*)aligned, 0, size);
    return (void*)aligned;
}

void Heap::Free(void* ptr)
{
    if (ptr == nullptr)
        return;

    std::lock_guard lock(mutex);
    o1heapFree(heap, *((void**)ptr - 1));
}

uint32_t Heap::AllocGuest(size_t size, size_t alignment)
{
    return g_memory.MapVirtual(Alloc(size, alignment));
}

void Heap::FreeGuest(uint32_t address)
{
    if (address != 0)
        Free(g_memory.Translate(address));
}
