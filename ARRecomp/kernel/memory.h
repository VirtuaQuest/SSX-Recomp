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
// The three physical views address the same 512 MB on real hardware. They are not aliased here; each physical
// allocation is only visible through the view it was allocated in.

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
        return host >= base && host < (base + PPC_MEMORY_SIZE);
    }

    void* Translate(size_t offset) const noexcept
    {
        if (offset)
            assert(offset < PPC_MEMORY_SIZE);

        return base + offset;
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

        return static_cast<uint32_t>(static_cast<const uint8_t*>(host) - base);
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

    PageAllocator(uint32_t begin, uint32_t end, uint32_t pageSize);

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
    std::map<uint32_t, Region> m_regions;
    mutable std::mutex m_mutex;
};

extern PageAllocator g_virtualAllocator;
extern PageAllocator g_physicalAllocator64K;
extern PageAllocator g_physicalAllocator16M;
extern PageAllocator g_physicalAllocator4K;

PageAllocator* FindAllocator(uint32_t address);
