#pragma once

// Guest address space layout. The whole 4 GB range is backed by one host allocation, so guest pointers translate
// to host pointers by adding the base.
//
//   0x00000000 - 0x00010000  unmapped, catches null pointer accesses
//   0x00010000 - 0x40000000  runtime heap (kernel objects, thread blocks and stacks)
//   0x40000000 - 0x7F000000  virtual memory allocated by the game (NtAllocateVirtualMemory)
//   0x7F000000 - 0x80000000  hardware registers (GPU, XMA)
//   0x82000000 - ...         executable image, followed by the recompiled function table
//   0xA0000000 - 0xC0000000  physical memory, 64 KB pages
//   0xC0000000 - 0xE0000000  physical memory, 16 MB pages
//   0xE0000000 - 0x100000000 physical memory, 4 KB pages
//
// The three physical views map the same 512 MB of physical memory, like on the console. Accesses at 0xE0000000
// and above are moved up by 4 KB (see ARRecompLib/ar_memory.h) to emulate that view's offset.

constexpr uint32_t GUEST_RUNTIME_HEAP_BEGIN = 0x00010000;
constexpr uint32_t GUEST_RUNTIME_HEAP_END = 0x40000000;
constexpr uint32_t GUEST_VIRTUAL_BEGIN = 0x40000000;
constexpr uint32_t GUEST_VIRTUAL_END = 0x7F000000;
constexpr uint32_t GUEST_PHYSICAL_SIZE = 0x20000000;

struct Memory
{
    uint8_t* base{};

    Memory();

    bool IsInMemoryRange(const void* host) const noexcept
    {
        return host >= base && host < (base + PPC_MEMORY_SIZE + 0x1000);
    }

    void* Translate(size_t offset) const noexcept
    {
        if (offset)
            assert(offset < PPC_MEMORY_SIZE);

        return base + PPCHostOffset(uint32_t(offset));
    }

    template<typename T>
    T* Translate(size_t offset) const noexcept
    {
        return offset ? reinterpret_cast<T*>(Translate(offset)) : nullptr;
    }

    uint32_t MapVirtual(const void* host) const noexcept
    {
        if (host)
            assert(IsInMemoryRange(host));

        // Inverse of PPCHostOffset: host memory from 0xE0001000 up belongs to guest addresses 4 KB lower.
        uint64_t offset = uint64_t(static_cast<const uint8_t*>(host) - base);
        return static_cast<uint32_t>(offset >= 0xE0001000ull ? offset - 0x1000 : offset);
    }

    PPCFunc* FindFunction(uint32_t guest) const noexcept
    {
        return PPC_LOOKUP_FUNC(base, guest);
    }

    void InsertFunction(uint32_t guest, PPCFunc* host)
    {
        PPC_LOOKUP_FUNC(base, guest) = host;
    }
};

extern "C" void* MmGetHostAddress(uint32_t ptr);
extern Memory g_memory;

// Page granular allocator for a guest address range, used for the game's virtual and physical allocations.
class PageAllocator
{
public:
    struct Region
    {
        uint32_t base;
        uint32_t size;
        uint32_t protect;
        bool committed;
    };

    // hostOffset is added to addresses to reach the memory, for allocators that manage physical addresses.
    PageAllocator(uint32_t begin, uint32_t end, uint32_t pageSize, uint32_t hostOffset = 0);

    // Allocates anywhere in the range, or at a fixed address when address is not 0. Returns 0 on failure.
    uint32_t Allocate(uint32_t address, uint32_t size, uint32_t alignment, uint32_t protect, bool commit, bool topDown = false);
    bool Free(uint32_t address, uint32_t* freedSize = nullptr);
    bool Commit(uint32_t address, uint32_t size, uint32_t protect);
    bool Decommit(uint32_t address, uint32_t size);
    std::optional<Region> Query(uint32_t address) const;
    bool Contains(uint32_t address) const { return address >= m_begin && address < m_end; }
    uint32_t PageSize() const { return m_pageSize; }
    uint64_t AllocatedBytes() const;

private:
    uint32_t m_begin;
    uint32_t m_end;
    uint32_t m_pageSize;
    uint32_t m_hostOffset;
    std::map<uint32_t, Region> m_regions;
    mutable std::mutex m_mutex;
};

extern PageAllocator g_virtualAllocator;
extern PageAllocator g_physicalAllocator; // Manages physical addresses (0 - 512 MB).

constexpr uint32_t PHYSICAL_VIEW_4K = 0xE0000000;
constexpr uint32_t PHYSICAL_VIEW_64K = 0xA0000000;
constexpr uint32_t PHYSICAL_VIEW_16M = 0xC0000000;

inline bool IsPhysicalAddress(uint32_t address)
{
    return address >= PHYSICAL_VIEW_64K;
}

// Physical memory allocation in one of the views; returns the virtual address.
uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment, uint32_t view, uint32_t protect, bool topDown);
void FreePhysicalMemory(uint32_t address);

// Virtual address in a physical view to the physical address, as used by the GPU and DMA engines.
inline uint32_t VirtualToPhysical(uint32_t address)
{
    if (!IsPhysicalAddress(address))
        return address;

    uint32_t physical = address & (GUEST_PHYSICAL_SIZE - 1);
    if (address >= PHYSICAL_VIEW_4K)
        physical = (physical + 0x1000) & (GUEST_PHYSICAL_SIZE - 1);

    return physical;
}

// Physical address to a virtual address in the 64 KB page view, which maps physical memory without an offset.
inline uint32_t PhysicalToVirtual(uint32_t physical)
{
    return PHYSICAL_VIEW_64K + (physical & (GUEST_PHYSICAL_SIZE - 1));
}
