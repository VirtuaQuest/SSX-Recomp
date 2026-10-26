#include <stdafx.h>
#include "memory.h"

constexpr uint32_t E_VIEW_OFFSET = 0x1000;

// Ranges of the physical memory backing allocated through the 0xE0000000 view, by start offset.
static std::mutex g_eViewMutex;
static std::map<uint32_t, uint32_t> g_eViewRanges;

static bool InEViewRange(uint32_t offset)
{
    std::lock_guard lock(g_eViewMutex);

    auto it = g_eViewRanges.upper_bound(offset);
    if (it == g_eViewRanges.begin())
        return false;

    --it;
    return offset < it->first + it->second;
}

uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment, uint32_t view, uint32_t protect, bool topDown)
{
    // E view allocations get a guard page, so a GPU address one page past their end is never also inside the
    // next allocation.
    bool eView = view == PHYSICAL_VIEW_4K;
    uint32_t offset = g_physicalAllocator.Allocate(0, eView ? size + E_VIEW_OFFSET : size, alignment, protect, true, topDown);
    if (offset == 0)
        return 0;

    if (eView)
    {
        std::lock_guard lock(g_eViewMutex);
        g_eViewRanges[offset] = size;
    }

    return view + offset;
}

void FreePhysicalMemory(uint32_t address)
{
    uint32_t offset = PhysicalMemoryOffset(address);
    if ((address & 0xE0000000) == PHYSICAL_VIEW_4K)
    {
        std::lock_guard lock(g_eViewMutex);
        g_eViewRanges.erase(offset);
    }

    g_physicalAllocator.Free(offset);
}

uint32_t VirtualToPhysical(uint32_t address)
{
    if (!IsPhysicalAddress(address))
        return address;

    uint32_t offset = PhysicalMemoryOffset(address);
    return (address & 0xE0000000) == PHYSICAL_VIEW_4K ? offset + E_VIEW_OFFSET : offset;
}

uint32_t PhysicalToVirtual(uint32_t physical)
{
    physical &= GUEST_PHYSICAL_SIZE - 1;
    if (physical >= E_VIEW_OFFSET && InEViewRange(physical - E_VIEW_OFFSET))
        physical -= E_VIEW_OFFSET;

    return PHYSICAL_VIEW_64K + physical;
}
