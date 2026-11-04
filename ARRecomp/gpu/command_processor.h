#pragma once

// Xenos command processor.
//
// The title's Direct3D library writes PM4 packets into a ring buffer and advances the write pointer register;
// the GPU consumes them and reports its progress through a read pointer it writes back to memory. This processor
// executes the packets that affect memory the CPU observes (memory writes, fences, scratch registers, waits,
// interrupts) and skips drawing, so the title's rendering code runs as if a GPU were attached. Drawing support
// can be added to the packet handlers later.
//
// GPU registers are memory mapped at 0x7FC80000 and accessed by the CPU as big endian words. They are kept in
// guest memory there, so CPU reads and writes need no special handling.
namespace gpu
{
    constexpr uint32_t REGISTER_BASE = 0x7FC80000;

    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Number of vertical blanks so far; also written by EVENT_WRITE_SHD packets that request the counter.
    uint32_t SwapCounter();

    // Called by VdSwap, to measure the GPU's latency behind the CPU.
    void NoteSwapSubmitted();

    // Frames the title has submitted (VdSwap calls): the CPU side frame number.
    uint32_t SubmittedSwaps();

    // Diagnostics: the ID of the last draw marker the CPU side inserted into the command stream (AR_MARK_DRAWS).
    uint32_t CurrentDrawMarker();
    uint32_t CurrentDrawMarkerObject();
    std::string TakeFetch0Log();

    // Prints the command processor state, for diagnosing hangs.
    void DumpState();

    // Diagnostics: the indirect buffers (guest physical address and size in words) the packet being executed was
    // reached through, outermost first. Returns how many.
    uint32_t GetIndirectBufferChain(uint32_t* addresses, uint32_t* sizes, uint32_t capacity);
}
