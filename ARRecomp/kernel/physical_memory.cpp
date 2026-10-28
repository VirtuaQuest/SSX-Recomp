#include <stdafx.h>
#include "memory.h"

// The physical allocator manages physical addresses. Allocations in the 0xE0000000 view start one page lower in
// virtual memory than their physical address; their physical base is kept for freeing, as aligning the virtual
// address can move it.
static std::mutex g_eViewMutex;
static std::unordered_map<uint32_t, uint32_t> g_eViewAllocations;

uint32_t AllocatePhysicalMemoryUnlogged(uint32_t size, uint32_t alignment, uint32_t view, uint32_t protect, bool topDown);

// Debugging (AR_WATCH_PHYSICAL): logs allocations and frees covering the watched physical address.
static void LogWatchedRange(const char* operation, uint32_t physical, uint32_t size, uint32_t address)
{
    static const uint32_t watched = getenv("AR_WATCH_PHYSICAL") ? uint32_t(strtoul(getenv("AR_WATCH_PHYSICAL"), nullptr, 16)) : 0;
    if (watched != 0 && watched >= physical && watched - physical < size)
        LOGF_WARNING("{} physical 0x{:08X}-0x{:08X} at 0x{:08X} covers the watched address", operation, physical, physical + size, address);
}

uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment, uint32_t view, uint32_t protect, bool topDown)
{
    uint32_t result = AllocatePhysicalMemoryUnlogged(size, alignment, view, protect, topDown);
    if (result != 0)
        LogWatchedRange("allocated", VirtualToPhysical(result), size, result);
    return result;
}

uint32_t AllocatePhysicalMemoryUnlogged(uint32_t size, uint32_t alignment, uint32_t view, uint32_t protect, bool topDown)
{
    alignment = std::max<uint32_t>(alignment, 0x1000);

    if (view != PHYSICAL_VIEW_4K)
    {
        uint32_t physical = g_physicalAllocator.Allocate(0, size, alignment, protect, true, topDown);
        return physical != 0 ? view + physical : 0;
    }

    // Virtual 0xE0000000 + X is physical X + 0x1000: find a physical address one page past an aligned virtual one.
    uint32_t physical = g_physicalAllocator.Allocate(0, size + alignment, 0x1000, protect, true, topDown);
    if (physical == 0)
        return 0;

    uint32_t aligned = physical + ((0x1000 - physical) & (alignment - 1));
    uint32_t address = view + aligned - 0x1000;

    std::lock_guard lock(g_eViewMutex);
    g_eViewAllocations[address] = physical;
    return address;
}

void FreePhysicalMemory(uint32_t address)
{
    LogWatchedRange("freeing", VirtualToPhysical(address), 0x100000, address);
    if (address >= PHYSICAL_VIEW_4K)
    {
        std::lock_guard lock(g_eViewMutex);
        auto it = g_eViewAllocations.find(address);
        if (it != g_eViewAllocations.end())
        {
            g_physicalAllocator.Free(it->second);
            g_eViewAllocations.erase(it);
        }

        return;
    }

    g_physicalAllocator.Free(VirtualToPhysical(address));
}
