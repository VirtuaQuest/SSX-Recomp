#include <stdafx.h>
#include "memory.h"

// Based on Unleashed Recompiled's kernel/memory.cpp (GPL-3.0).

#ifdef _WIN32
// Reserves the 4 GB guest address space as placeholders, commits the range below the physical views as ordinary
// memory and maps one 512 MB section at each of the three physical views, so they alias like on the console.
// VirtualAlloc2 and MapViewOfFile3 are looked up at runtime as they are missing from older import libraries.
static uint8_t* ReserveGuestMemory()
{
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
    auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
    if (virtualAlloc2 == nullptr || mapViewOfFile3 == nullptr)
        return nullptr;

    // One extra 64 KB block past 4 GB, as accesses at the top of the 0xE0000000 view land up to 4 KB past it.
    constexpr uint64_t RESERVED_SIZE = PPC_MEMORY_SIZE + 0x10000;

    HANDLE process = GetCurrentProcess();
    uint8_t* reserved = nullptr;
    for (void* preferred : { (void*)0x100000000ull, (void*)nullptr })
    {
        reserved = (uint8_t*)virtualAlloc2(process, preferred, RESERVED_SIZE, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (reserved != nullptr)
            break;
    }

    if (reserved == nullptr)
        return nullptr;

    constexpr uint64_t physicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

    // Split the placeholder into the ordinary range and the three views.
    VirtualFree(reserved, physicalViews[0], MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    VirtualFree(reserved + physicalViews[0], GUEST_PHYSICAL_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    VirtualFree(reserved + physicalViews[1], GUEST_PHYSICAL_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    VirtualFree(reserved + physicalViews[2], GUEST_PHYSICAL_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);

    if (virtualAlloc2(process, reserved, physicalViews[0], MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr)
        return nullptr;

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, GUEST_PHYSICAL_SIZE, nullptr);
    if (section == nullptr)
        return nullptr;

    for (uint64_t view : physicalViews)
    {
        if (mapViewOfFile3(section, process, reserved + view, 0, GUEST_PHYSICAL_SIZE, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr)
            return nullptr;
    }

    // Past the end, physical addresses wrap around.
    if (mapViewOfFile3(section, process, reserved + PPC_MEMORY_SIZE, 0, 0x10000, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr)
        return nullptr;

    return reserved;
}
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = ReserveGuestMemory();
    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, GUEST_RUNTIME_HEAP_BEGIN, PAGE_NOACCESS, &oldProtect);
#else
#error Only Windows is supported for now.
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}

PageAllocator::PageAllocator(uint32_t begin, uint32_t end, uint32_t pageSize, uint32_t hostOffset)
    : m_begin(begin), m_end(end), m_pageSize(pageSize), m_hostOffset(hostOffset)
{
}

static uint32_t AlignUp(uint64_t value, uint64_t alignment)
{
    return uint32_t((value + alignment - 1) & ~(alignment - 1));
}

uint32_t PageAllocator::Allocate(uint32_t address, uint32_t size, uint32_t alignment, uint32_t protect, bool commit, bool topDown)
{
    std::lock_guard lock(m_mutex);

    size = AlignUp(std::max<uint32_t>(size, 1), m_pageSize);
    alignment = std::max(alignment, m_pageSize);

    if (address != 0)
    {
        address &= ~(m_pageSize - 1);
        if (address < m_begin || uint64_t(address) + size > m_end)
            return 0;

        auto next = m_regions.lower_bound(address);
        if (next != m_regions.end() && next->first < address + size)
            return 0;
        if (next != m_regions.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second.size > address)
                return 0;
        }
    }
    else if (!topDown)
    {
        uint64_t candidate = AlignUp(m_begin, alignment);
        for (const auto& [regionBase, region] : m_regions)
        {
            if (candidate + size <= regionBase)
                break;

            candidate = AlignUp(uint64_t(regionBase) + region.size, alignment);
        }

        if (candidate + size > m_end)
            return 0;

        address = uint32_t(candidate);
    }
    else
    {
        uint64_t limit = m_end;
        bool found = false;
        for (auto it = m_regions.rbegin(); ; ++it)
        {
            uint64_t gapBegin = it == m_regions.rend() ? m_begin : uint64_t(it->first) + it->second.size;
            if (limit >= size)
            {
                uint64_t candidate = (limit - size) & ~uint64_t(alignment - 1);
                if (candidate >= gapBegin)
                {
                    address = uint32_t(candidate);
                    found = true;
                    break;
                }
            }

            if (it == m_regions.rend())
                break;

            limit = it->first;
        }

        if (!found)
            return 0;
    }

    m_regions.emplace(address, Region{ address, size, protect, commit });

    // Memory handed out by the kernel is always zeroed.
    memset(g_memory.Translate(uint64_t(address) + m_hostOffset), 0, size);
    return address;
}

bool PageAllocator::Free(uint32_t address, uint32_t* freedSize)
{
    std::lock_guard lock(m_mutex);

    auto it = m_regions.find(address & ~(m_pageSize - 1));
    if (it == m_regions.end())
        return false;

    if (freedSize != nullptr)
        *freedSize = it->second.size;

    m_regions.erase(it);
    return true;
}

bool PageAllocator::Commit(uint32_t address, uint32_t size, uint32_t protect)
{
    std::lock_guard lock(m_mutex);

    auto it = m_regions.upper_bound(address);
    if (it == m_regions.begin())
        return false;

    auto& region = std::prev(it)->second;
    if (uint64_t(address) + size > uint64_t(region.base) + region.size)
        return false;

    region.committed = true;
    region.protect = protect;
    return true;
}

bool PageAllocator::Decommit(uint32_t address, uint32_t size)
{
    std::lock_guard lock(m_mutex);

    auto it = m_regions.upper_bound(address);
    if (it == m_regions.begin())
        return false;

    auto& region = std::prev(it)->second;
    uint32_t begin = address & ~(m_pageSize - 1);
    uint32_t end = std::min<uint64_t>(AlignUp(uint64_t(address) + size, m_pageSize), uint64_t(region.base) + region.size);
    if (begin < end)
        memset(g_memory.Translate(uint64_t(begin) + m_hostOffset), 0, end - begin);

    return true;
}

std::optional<PageAllocator::Region> PageAllocator::Query(uint32_t address) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_regions.upper_bound(address);
    if (it == m_regions.begin())
        return std::nullopt;

    const auto& region = std::prev(it)->second;
    if (address >= uint64_t(region.base) + region.size)
        return std::nullopt;

    return region;
}

uint64_t PageAllocator::AllocatedBytes() const
{
    std::lock_guard lock(m_mutex);

    uint64_t total = 0;
    for (const auto& [regionBase, region] : m_regions)
        total += region.size;

    return total;
}

PageAllocator g_virtualAllocator(GUEST_VIRTUAL_BEGIN, GUEST_VIRTUAL_END, 0x1000);

// Starts at 64 KB so that physical address 0 is never handed out.
PageAllocator g_physicalAllocator(0x10000, GUEST_PHYSICAL_SIZE, 0x1000, PHYSICAL_VIEW_64K);
