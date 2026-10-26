// Virtual and physical memory.
#include <stdafx.h>
#include <kernel/imports.h>

constexpr uint32_t X_MEM_COMMIT = 0x00001000;
constexpr uint32_t X_MEM_RESERVE = 0x00002000;
constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
constexpr uint32_t X_MEM_RELEASE = 0x00008000;
constexpr uint32_t X_MEM_FREE = 0x00010000;
constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;

constexpr uint32_t X_PAGE_NOACCESS = 0x01;
constexpr uint32_t X_PAGE_READWRITE = 0x04;

// The allocator responsible for a guest address, and the address in that allocator's terms.
struct AllocationTarget
{
    PageAllocator* allocator;
    uint32_t address;
    uint32_t viewOffset; // Added to the allocator's addresses to form guest addresses.
};

static std::optional<AllocationTarget> FindTarget(uint32_t address)
{
    if (IsPhysicalAddress(address))
    {
        uint32_t physical = VirtualToPhysical(address);
        return AllocationTarget{ &g_physicalAllocator, physical, address - physical };
    }

    if (g_virtualAllocator.Contains(address))
        return AllocationTarget{ &g_virtualAllocator, address, 0 };

    return std::nullopt;
}

namespace imports
{
    uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t allocationType,
        uint32_t protect, uint32_t debugMemory)
    {
        if (baseAddress == nullptr || regionSize == nullptr || *regionSize == 0)
            return X_STATUS_INVALID_PARAMETER;

        uint32_t address = *baseAddress;
        uint32_t size = *regionSize;
        uint32_t alignment = (allocationType & X_MEM_LARGE_PAGES) ? 0x10000 : 0x1000;

        // Committing inside an existing reservation.
        if (address != 0 && (allocationType & X_MEM_COMMIT) && !(allocationType & X_MEM_RESERVE))
        {
            auto target = FindTarget(address);
            if (target.has_value() && target->allocator->Commit(target->address, size, protect))
            {
                uint32_t begin = address & ~(alignment - 1);
                *baseAddress = begin;
                *regionSize = ((address + size - begin) + alignment - 1) & ~(alignment - 1);
                return X_STATUS_SUCCESS;
            }
        }

        uint32_t allocated = g_virtualAllocator.Allocate(address, size, alignment, protect,
            (allocationType & X_MEM_COMMIT) != 0, (allocationType & X_MEM_TOP_DOWN) != 0);

        if (allocated == 0)
        {
            LOGF_WARNING("failed to allocate 0x{:X} bytes at 0x{:08X} (type 0x{:X})", size, address, allocationType);
            return address != 0 ? X_STATUS_CONFLICTING_ADDRESSES : X_STATUS_NO_MEMORY;
        }

        *baseAddress = allocated;
        *regionSize = (size + alignment - 1) & ~(alignment - 1);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t debugMemory)
    {
        if (baseAddress == nullptr)
            return X_STATUS_INVALID_PARAMETER;

        auto target = FindTarget(*baseAddress);
        if (!target.has_value())
            return X_STATUS_MEMORY_NOT_ALLOCATED;

        if (freeType & X_MEM_DECOMMIT)
        {
            uint32_t size = regionSize != nullptr ? regionSize->get() : 0;
            if (!target->allocator->Decommit(target->address, size))
                return X_STATUS_MEMORY_NOT_ALLOCATED;

            return X_STATUS_SUCCESS;
        }

        uint32_t freed = 0;
        if (!target->allocator->Free(target->address, &freed))
            return X_STATUS_MEMORY_NOT_ALLOCATED;

        if (regionSize != nullptr)
            *regionSize = freed;

        return X_STATUS_SUCCESS;
    }

    struct XMemoryBasicInformation
    {
        be<uint32_t> baseAddress;
        be<uint32_t> allocationBase;
        be<uint32_t> allocationProtect;
        be<uint32_t> regionSize;
        be<uint32_t> state;
        be<uint32_t> protect;
        be<uint32_t> type;
    };

    uint32_t NtQueryVirtualMemory(uint32_t baseAddress, XMemoryBasicInformation* info)
    {
        auto target = FindTarget(baseAddress);
        auto region = target.has_value() ? target->allocator->Query(target->address) : std::nullopt;

        memset(info, 0, sizeof(*info));
        if (!region.has_value())
        {
            info->baseAddress = baseAddress & ~0xFFFu;
            info->regionSize = 0x1000;
            info->state = X_MEM_FREE;
            info->protect = X_PAGE_NOACCESS;
            return X_STATUS_SUCCESS;
        }

        info->baseAddress = baseAddress & ~0xFFFu;
        info->allocationBase = region->base + target->viewOffset;
        info->allocationProtect = region->protect;
        info->regionSize = region->base + target->viewOffset + region->size - info->baseAddress;
        info->state = region->committed ? X_MEM_COMMIT : X_MEM_RESERVE;
        info->protect = region->protect;
        info->type = X_MEM_PRIVATE;
        return X_STATUS_SUCCESS;
    }

    uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect, uint32_t minAddress,
        uint32_t maxAddress, uint32_t alignment)
    {
        // The page size picks the view the memory is returned in.
        uint32_t view = PHYSICAL_VIEW_4K;
        uint32_t pageSize = 0x1000;
        if (protect & X_MEM_16MB_PAGES)
        {
            view = PHYSICAL_VIEW_16M;
            pageSize = 0x1000000;
        }
        else if (protect & X_MEM_LARGE_PAGES)
        {
            view = PHYSICAL_VIEW_64K;
            pageSize = 0x10000;
        }

        size = (size + pageSize - 1) & ~(pageSize - 1);
        uint32_t address = AllocatePhysicalMemory(size, std::max(alignment, pageSize), view, protect, (flags & 1) != 0);
        if (address == 0)
            LOGF_ERROR("failed to allocate 0x{:X} bytes of physical memory (protect 0x{:X})", size, protect);

        return address;
    }

    void MmFreePhysicalMemory(uint32_t type, uint32_t baseAddress)
    {
        FreePhysicalMemory(baseAddress);
    }

    uint32_t MmGetPhysicalAddress(uint32_t address)
    {
        return VirtualToPhysical(address);
    }

    uint32_t MmQueryAddressProtect(uint32_t address)
    {
        auto target = FindTarget(address);
        auto region = target.has_value() ? target->allocator->Query(target->address) : std::nullopt;
        if (region.has_value())
            return region->protect;

        // The executable image and runtime memory are readable and writable.
        return address != 0 ? X_PAGE_READWRITE : 0;
    }

    uint32_t MmMapIoSpace(uint32_t unknown, uint32_t sourceAddress, uint32_t size, uint32_t flags)
    {
        return sourceAddress;
    }

    // MM_STATISTICS, all page counts in 4 KB pages.
    struct XMmStatistics
    {
        be<uint32_t> length;
        be<uint32_t> totalPhysicalPages;
        be<uint32_t> kernelPages;
        be<uint32_t> titleAvailablePages;
        be<uint32_t> titleTotalVirtualMemoryBytes;
        be<uint32_t> titleReservedVirtualMemoryBytes;
        be<uint32_t> titlePhysicalPages;
        be<uint32_t> titlePoolPages;
        be<uint32_t> titleStackPages;
        be<uint32_t> titleImagePages;
        be<uint32_t> titleHeapPages;
        be<uint32_t> titleVirtualPages;
        be<uint32_t> titlePageTablePages;
        be<uint32_t> titleCachePages;
        be<uint32_t> systemAvailablePages;
        be<uint32_t> systemTotalVirtualMemoryBytes;
        be<uint32_t> systemReservedVirtualMemoryBytes;
        be<uint32_t> systemPhysicalPages;
        be<uint32_t> systemPoolPages;
        be<uint32_t> systemStackPages;
        be<uint32_t> systemImagePages;
        be<uint32_t> systemHeapPages;
        be<uint32_t> systemVirtualPages;
        be<uint32_t> systemPageTablePages;
        be<uint32_t> systemCachePages;
        be<uint32_t> highestPhysicalPage;
    };

    uint32_t MmQueryStatistics(XMmStatistics* stats)
    {
        if (stats == nullptr || stats->length != sizeof(XMmStatistics))
            return X_STATUS_BUFFER_TOO_SMALL;

        uint64_t physicalUsed = g_physicalAllocator.AllocatedBytes();
        uint64_t virtualUsed = g_virtualAllocator.AllocatedBytes();
        uint32_t totalPages = GUEST_PHYSICAL_SIZE / 0x1000;
        uint32_t usedPages = uint32_t((physicalUsed + virtualUsed) / 0x1000);

        memset(reinterpret_cast<uint8_t*>(stats) + 4, 0, sizeof(XMmStatistics) - 4);
        stats->totalPhysicalPages = totalPages;
        stats->kernelPages = 0x2000;
        stats->titleAvailablePages = usedPages < totalPages - 0x2000 ? totalPages - 0x2000 - usedPages : 0;
        stats->titleTotalVirtualMemoryBytes = GUEST_VIRTUAL_END - GUEST_VIRTUAL_BEGIN;
        stats->titleReservedVirtualMemoryBytes = uint32_t(virtualUsed);
        stats->titlePhysicalPages = uint32_t(physicalUsed / 0x1000);
        stats->titleVirtualPages = uint32_t(virtualUsed / 0x1000);
        stats->highestPhysicalPage = totalPages - 1;
        return X_STATUS_SUCCESS;
    }

    uint32_t ExAllocatePoolTypeWithTag(uint32_t size, uint32_t tag, uint32_t zero)
    {
        return g_runtimeHeap.AllocGuest(size, 0x10);
    }

    void ExFreePool(uint32_t address)
    {
        g_runtimeHeap.FreeGuest(address);
    }
}

AR_IMPORT(NtAllocateVirtualMemory);
AR_IMPORT(NtFreeVirtualMemory);
AR_IMPORT(NtQueryVirtualMemory);
AR_IMPORT(MmAllocatePhysicalMemoryEx);
AR_IMPORT(MmFreePhysicalMemory);
AR_IMPORT(MmGetPhysicalAddress);
AR_IMPORT(MmQueryAddressProtect);
AR_IMPORT(MmMapIoSpace);
AR_IMPORT(MmQueryStatistics);
AR_IMPORT(ExAllocatePoolTypeWithTag);
AR_IMPORT(ExFreePool);
