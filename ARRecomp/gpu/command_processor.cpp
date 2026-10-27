#include <stdafx.h>
#include "command_processor.h"
#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include "registers.h"
#include "renderer.h"

namespace gpu
{
    using namespace reg;

    // PM4 type 3 opcodes.
    enum : uint32_t
    {
        PM4_REG_RMW = 0x21,
        PM4_DRAW_INDX = 0x22,
        PM4_IM_LOAD = 0x27,
        PM4_IM_LOAD_IMMEDIATE = 0x2B,
        PM4_SET_CONSTANT = 0x2D,
        PM4_LOAD_ALU_CONSTANT = 0x2F,
        PM4_DRAW_INDX_2 = 0x36,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_WAIT_REG_MEM = 0x3C,
        PM4_MEM_WRITE = 0x3D,
        PM4_INDIRECT_BUFFER = 0x3F,
        PM4_COND_WRITE = 0x45,
        PM4_EVENT_WRITE = 0x46,
        PM4_INTERRUPT = 0x54,
        PM4_EVENT_WRITE_SHD = 0x58,
        PM4_EVENT_WRITE_EXT = 0x59,
        PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_SET_CONSTANT2 = 0x55,
        PM4_SET_SHADER_CONSTANTS = 0x56,
        PM4_XE_SWAP = 0x64, // Written by VdSwap.
    };

    static std::atomic<uint32_t> g_ringBase;      // Guest virtual address.
    static std::atomic<uint32_t> g_ringSizeWords;
    static std::atomic<uint32_t> g_readPointerWriteBack; // Physical address.
    static std::atomic<uint32_t> g_interruptCallback;
    static std::atomic<uint32_t> g_interruptUserData;
    static std::atomic<uint32_t> g_swapCounter;
    static std::atomic<uint64_t> g_frames;
    static uint32_t g_readPointer;

    uint32_t SwapCounter()
    {
        return g_swapCounter;
    }

    static be<uint32_t>* Registers()
    {
        return g_memory.Translate<be<uint32_t>>(REGISTER_BASE);
    }

    static uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF); // 8 in 16
        case 2: return ByteSwap(value);                                           // 8 in 32
        case 3: return (value << 16) | (value >> 16);                              // 16 in 32
        default: return value;
        }
    }

    // GPU memory accesses use physical addresses whose low two bits select the byte order.
    static void WriteMemory(uint32_t address, uint32_t value)
    {
        uint32_t data = GpuSwap(value, address & 3);
        memcpy(g_memory.Translate(PhysicalToVirtual(address & ~3u)), &data, sizeof(data));
    }

    static uint32_t ReadMemory(uint32_t address)
    {
        uint32_t data;
        memcpy(&data, g_memory.Translate(PhysicalToVirtual(address & ~3u)), sizeof(data));
        return GpuSwap(data, address & 3);
    }

    // ---- Interrupts ----

    // Interrupt callbacks are guest code, so they run on a guest thread, one at a time. Each interrupt targets a
    // hardware thread; the callback reads the processor number from the PCR (handlers acknowledge per processor),
    // so the interrupt thread's PCR is set to the target before each call.
    static std::mutex g_interruptMutex;
    static std::condition_variable g_interruptCondition;
    static std::vector<std::pair<uint32_t, uint32_t>> g_interruptQueue;

    static void QueueInterrupt(uint32_t source, uint32_t cpu)
    {
        std::lock_guard lock(g_interruptMutex);
        g_interruptQueue.emplace_back(source, cpu);
        g_interruptCondition.notify_one();
    }

    static void RunInterruptThread()
    {
        uint8_t* processorNumber = g_memory.Translate<uint8_t>(GetCurrentThreadObject()->pcr + 0x10C);

        while (true)
        {
            std::pair<uint32_t, uint32_t> interrupt;
            {
                std::unique_lock lock(g_interruptMutex);
                g_interruptCondition.wait(lock, [] { return !g_interruptQueue.empty(); });
                interrupt = g_interruptQueue.front();
                g_interruptQueue.erase(g_interruptQueue.begin());
            }

            if (uint32_t callback = g_interruptCallback)
            {
                *processorNumber = uint8_t(interrupt.second);
                GuestToHostFunction<void>(callback, interrupt.first, g_interruptUserData.load());
            }
        }
    }

    static void RunVblankThread()
    {
        auto next = std::chrono::steady_clock::now();
        while (true)
        {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);

            g_swapCounter++;
            QueueInterrupt(0, 2); // Vertical blank interrupts go to the third hardware thread.
        }
    }

    // ---- Packet processing ----

    static void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= REGISTER_COUNT)
            return;

        auto* registers = Registers();
        registers[index] = value;

        if (index == SCRATCH_ADDR || index == SCRATCH_UMSK || (index >= SCRATCH_REG0 && index < SCRATCH_REG0 + 8))
            LOGF_TRACE("register 0x{:04X} = 0x{:08X}", index, value);

        if (index >= SCRATCH_REG0 && index < SCRATCH_REG0 + 8)
        {
            // Scratch registers selected by the mask are mirrored to memory, which the CPU polls for fences.
            uint32_t scratch = index - SCRATCH_REG0;
            if ((registers[SCRATCH_UMSK] >> scratch) & 1)
                *g_memory.Translate<be<uint32_t>>(PhysicalToVirtual(registers[SCRATCH_ADDR] + scratch * 4)) = value;
        }
        else if (index == COHER_STATUS_HOST && (value & 0x80000000))
        {
            // Cache coherency requests complete immediately.
            registers[index] = value & ~0x80000000u;
        }
    }

    // Reads packet words from the ring buffer (wrapping) or from an indirect buffer.
    class PacketReader
    {
    public:
        PacketReader(uint32_t base, uint32_t sizeWords, uint32_t position, bool wraps)
            : m_base(base), m_size(sizeWords), m_position(position), m_wraps(wraps)
        {
        }

        uint32_t Read()
        {
            uint32_t value = g_memory.Translate<be<uint32_t>>(m_base)[m_position];
            m_position++;
            if (m_wraps && m_position == m_size)
                m_position = 0;

            return value;
        }

        void Skip(uint32_t count)
        {
            m_position = m_wraps ? (m_position + count) % m_size : m_position + count;
        }

        uint32_t Position() const { return m_position; }
        bool Wraps() const { return m_wraps; }

    private:
        uint32_t m_base;
        uint32_t m_size;
        uint32_t m_position;
        bool m_wraps;
    };

    static bool Compare(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
        case 0: return false;
        case 1: return value < reference;
        case 2: return value <= reference;
        case 3: return value == reference;
        case 4: return value != reference;
        case 5: return value >= reference;
        case 6: return value > reference;
        default: return true;
        }
    }

    static uint32_t ReadPollValue(uint32_t waitInfo, uint32_t address)
    {
        if (waitInfo & 0x10)
            return ReadMemory(address);

        return address < REGISTER_COUNT ? Registers()[address].get() : 0;
    }

    static void ExecuteBuffer(PacketReader& reader, uint32_t endPosition);

    // SET_CONSTANT and LOAD_ALU_CONSTANT address one of the constant blocks, or the context registers.
    static uint32_t ConstantRegister(uint32_t offsetType)
    {
        static constexpr uint32_t bases[] = { SHADER_CONSTANT_000_X, SHADER_CONSTANT_FETCH_00_0,
            SHADER_CONSTANT_BOOL_000_031, SHADER_CONSTANT_LOOP_00, 0x2000 };

        uint32_t type = (offsetType >> 16) & 0xFF;
        return type < std::size(bases) ? bases[type] + (offsetType & 0x7FF) : REGISTER_COUNT;
    }

    static void ExecuteDraw(PacketReader& reader, uint32_t& consumed, uint32_t count, bool hasVizQuery)
    {
        auto read = [&]() { consumed++; return reader.Read(); };
        if (hasVizQuery)
            read();

        DrawCall draw{};
        draw.initiator.value = read();
        WriteRegister(VGT_DRAW_INITIATOR, draw.initiator.value);

        if (draw.initiator.Source() == SourceSelect::DMA)
        {
            draw.indexBase = read() & 0x1FFFFFFF;
            uint32_t dmaSize = read();
            draw.indexEndian = dmaSize >> 30;
            draw.indexWords = dmaSize & 0xFFFFFF;
            WriteRegister(VGT_DMA_BASE, draw.indexBase);
            WriteRegister(VGT_DMA_SIZE, dmaSize);
        }
        else if (draw.initiator.Source() == SourceSelect::Immediate)
        {
            // Indices follow in the packet, packed as 16 or 32 bit values.
            uint32_t words = count - consumed;
            draw.immediateIndices.resize(words);
            for (uint32_t i = 0; i < words; i++)
                draw.immediateIndices[i] = read();
        }

        IssueDraw(Registers(), draw);
    }

    static int g_indirectDepth;

    static void ExecutePacket3(PacketReader& reader, uint32_t opcode, uint32_t count)
    {
        uint32_t start = reader.Position();
        uint32_t consumed = 0;
        auto read = [&]() { consumed++; return reader.Read(); };

        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t address = read() & ~3u;
            uint32_t length = read() & 0xFFFFF;
            LOGF_TRACE("indirect buffer 0x{:08X}, {} words", address, length);

            // Indirect buffers do not nest deeply on the console; deep nesting means misparsed packets.
            if (g_indirectDepth >= 4)
            {
                LOGF_ERROR("indirect buffers nested too deeply at 0x{:08X}", address);
                break;
            }

            g_indirectDepth++;
            PacketReader indirect(PhysicalToVirtual(address), length, 0, false);
            ExecuteBuffer(indirect, length);
            g_indirectDepth--;
            break;
        }

        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
            ExecuteDraw(reader, consumed, count, opcode == PM4_DRAW_INDX);
            break;

        case PM4_SET_CONSTANT:
        {
            uint32_t base = ConstantRegister(read());
            for (uint32_t i = 1; i < count; i++)
                WriteRegister(base + i - 1, read());
            break;
        }

        case PM4_SET_CONSTANT2:
        case PM4_SET_SHADER_CONSTANTS:
        {
            uint32_t base = read() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++)
                WriteRegister(base + i - 1, read());
            break;
        }

        case PM4_LOAD_ALU_CONSTANT:
        {
            uint32_t address = read() & 0x3FFFFFFF;
            uint32_t base = ConstantRegister(read());
            uint32_t sizeWords = read() & 0xFFF;
            auto* source = g_memory.Translate<be<uint32_t>>(PhysicalToVirtual(address));
            for (uint32_t i = 0; i < sizeWords; i++)
                WriteRegister(base + i, source[i]);
            break;
        }

        case PM4_IM_LOAD:
        {
            uint32_t addressType = read();
            uint32_t startSize = read();
            auto* code = g_memory.Translate<be<uint32_t>>(PhysicalToVirtual(addressType & ~3u));
            LoadShader(addressType & 3, code, startSize & 0xFFFF);
            break;
        }

        case PM4_IM_LOAD_IMMEDIATE:
        {
            uint32_t type = read();
            uint32_t startSize = read();
            uint32_t sizeWords = std::min(startSize & 0xFFFF, count - consumed);
            std::vector<be<uint32_t>> code(sizeWords);
            for (uint32_t i = 0; i < sizeWords; i++)
                code[i] = read();
            LoadShader(type & 3, code.data(), sizeWords);
            break;
        }

        case PM4_MEM_WRITE:
        {
            uint32_t address = read();
            for (uint32_t i = 1; i < count; i++)
                WriteMemory(address + (i - 1) * 4, read());
            break;
        }

        case PM4_WAIT_REG_MEM:
        {
            uint32_t waitInfo = read();
            uint32_t pollAddress = read();
            uint32_t reference = read();
            uint32_t mask = read();
            read(); // Wait interval.

            // The CPU satisfies these waits (for example by writing memory); poll until it does.
            auto start = std::chrono::steady_clock::now();
            bool warned = false;
            while (!Compare(waitInfo, ReadPollValue(waitInfo, pollAddress) & mask, reference))
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
                {
                    LOGF_WARNING("GPU is waiting on {} 0x{:08X} (function {}, reference 0x{:X}, mask 0x{:X})",
                        (waitInfo & 0x10) ? "memory" : "register", pollAddress, waitInfo & 7, reference, mask);
                    warned = true;
                }
            }
            break;
        }

        case PM4_REG_RMW:
        {
            uint32_t info = read();
            uint32_t andMask = read();
            uint32_t orMask = read();
            auto* registers = Registers();
            uint32_t index = info & 0x1FFF;
            uint32_t value = index < REGISTER_COUNT ? registers[index].get() : 0;
            value &= (info >> 31) ? registers[andMask & 0x1FFF].get() : andMask;
            value |= ((info >> 30) & 1) ? registers[orMask & 0x1FFF].get() : orMask;
            WriteRegister(index, value);
            break;
        }

        case PM4_COND_WRITE:
        {
            uint32_t waitInfo = read();
            uint32_t pollAddress = read();
            uint32_t reference = read();
            uint32_t mask = read();
            uint32_t writeAddress = read();
            uint32_t data = read();
            if (Compare(waitInfo, ReadPollValue(waitInfo, pollAddress) & mask, reference))
            {
                if (waitInfo & 0x100)
                    WriteMemory(writeAddress, data);
                else
                    WriteRegister(writeAddress, data);
            }
            break;
        }

        case PM4_EVENT_WRITE:
            WriteRegister(VGT_EVENT_INITIATOR, read() & 0x3F);
            break;

        case PM4_EVENT_WRITE_SHD:
        {
            uint32_t initiator = read();
            uint32_t address = read();
            uint32_t value = read();
            WriteRegister(VGT_EVENT_INITIATOR, initiator & 0x3F);
            WriteMemory(address, (initiator >> 31) ? g_swapCounter.load() : value);
            LOGF_TRACE("event write 0x{:08X} to 0x{:08X} (initiator 0x{:08X})", value, address, initiator);
            break;
        }

        case PM4_EVENT_WRITE_EXT:
        {
            uint32_t initiator = read();
            uint32_t address = read();
            WriteRegister(VGT_EVENT_INITIATOR, initiator & 0x3F);

            // Screen extents (min/max x, y, z) in 16 bit units of 8 pixels: report the full render target.
            static const uint16_t extents[] = { 0, 1280 / 8, 0, 720 / 8, 0, 1 };
            auto* out = g_memory.Translate<be<uint16_t>>(PhysicalToVirtual(address & ~3u));
            for (size_t i = 0; i < std::size(extents); i++)
                out[i] = extents[i];
            break;
        }

        case PM4_EVENT_WRITE_ZPD:
        {
            static std::atomic<bool> warned;
            if (!warned.exchange(true))
                LOGF_WARNING("occlusion queries are not implemented");
            break;
        }

        case PM4_INTERRUPT:
        {
            uint32_t cpuMask = read();
            for (uint32_t cpu = 0; cpu < 6; cpu++)
            {
                if (cpuMask & (1u << cpu))
                    QueueInterrupt(1, cpu);
            }
            break;
        }

        case PM4_XE_SWAP:
        {
            uint64_t frame = ++g_frames;
            if (frame == 1 || frame % 600 == 0)
                LOGF_INFO("frame {}", frame);

            // Written by VdSwap: 'SWAP', the front buffer's physical address and size, then its fetch constant.
            SwapInfo swap{};
            read();
            swap.frontBuffer = read();
            swap.width = read();
            swap.height = read();
            for (auto& word : swap.fetchConstant)
                word = read();
            Swap(Registers(), swap);
            break;
        }

        default:
            break;
        }

        // Packets carry their length, so anything not consumed by the handler (or not handled) is skipped.
        if (consumed < count)
            reader.Skip(count - consumed);
    }

    static void ExecutePacket(PacketReader& reader)
    {
        uint32_t header = reader.Read();
        LOGF_TRACE("packet 0x{:08X} at {}", header, reader.Position() - 1);
        switch (header >> 30)
        {
        case 0:
        {
            // Register writes: count values starting at a base index, or all to one register.
            uint32_t base = header & 0x7FFF;
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            bool single = (header >> 15) & 1;
            for (uint32_t i = 0; i < count; i++)
                WriteRegister(single ? base : base + i, reader.Read());
            break;
        }

        case 1:
            WriteRegister(header & 0x7FF, reader.Read());
            WriteRegister((header >> 11) & 0x7FF, reader.Read());
            break;

        case 2:
            break;

        case 3:
            ExecutePacket3(reader, (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1);
            break;
        }
    }

    static void ExecuteBuffer(PacketReader& reader, uint32_t endPosition)
    {
        // Indirect buffers end at their length; a packet overrunning it ends the buffer.
        if (!reader.Wraps())
        {
            while (reader.Position() < endPosition)
                ExecutePacket(reader);
            return;
        }

        while (reader.Position() != endPosition)
            ExecutePacket(reader);
    }

    static void RunCommandProcessor()
    {
        while (true)
        {
            uint32_t base = g_ringBase;
            uint32_t size = g_ringSizeWords;
            uint32_t writePointer = Registers()[CP_RB_WPTR];

            if (base == 0 || size == 0 || writePointer == g_readPointer || writePointer >= size)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }

            PacketReader reader(base, size, g_readPointer, true);
            ExecuteBuffer(reader, writePointer);
            g_readPointer = writePointer;

            Registers()[CP_RB_RPTR] = g_readPointer;
            if (uint32_t writeBack = g_readPointerWriteBack)
                *g_memory.Translate<be<uint32_t>>(PhysicalToVirtual(writeBack)) = g_readPointer;
        }
    }

    // Display controller registers the title reads directly. The values match a 1280x720 output that is always
    // reporting a vertical blank, as Xenia does.
    static void InitializeDisplayRegisters()
    {
        auto* registers = Registers();
        registers[0x0F00] = 0x08100748; // RB_EDRAM_TIMING
        registers[0x0F01] = 0x0000200E; // RB_BC_CONTROL
        registers[0x194C] = 0x000002D0; // D1MODE_V_COUNTER
        registers[0x1951] = 0x00000001; // D1MODE_VBLANK_STATUS, checked by the vertical blank interrupt handler.
        registers[0x1961] = 0x050002D0; // D1MODE_VIEWPORT_SIZE
    }

    static void StartThreads()
    {
        static std::once_flag once;
        std::call_once(once, []
            {
                InitializeDisplayRegisters();

                for (auto [entry, name] : { std::pair{ &RunCommandProcessor, "GPU command processor" },
                    std::pair{ &RunInterruptThread, "GPU interrupts" }, std::pair{ &RunVblankThread, "GPU vblank" } })
                {
                    ThreadStartParams params{};
                    params.hostEntry = entry;
                    params.name = name;
                    CreateGuestThread(params);
                }
            });
    }

    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        g_readPointer = 0;
        Registers()[CP_RB_WPTR] = 0;
        g_ringSizeWords = (1u << (sizeLog2 + 3)) / 4;
        g_ringBase = PhysicalToVirtual(physicalAddress);
        LOGF_INFO("ring buffer at 0x{:08X}, {} words", physicalAddress, g_ringSizeWords.load());
        StartThreads();
    }

    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2)
    {
        g_readPointerWriteBack = IsPhysicalAddress(physicalAddress) ? VirtualToPhysical(physicalAddress) : physicalAddress;
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        g_interruptCallback = callback;
        g_interruptUserData = userData;
        StartThreads();
    }
}

namespace gpu
{
    void DumpState()
    {
        LOGF_INFO("GPU: ring 0x{:08X} ({} words), read pointer {}, write pointer {}, frames {}, vblanks {}",
            g_ringBase.load(), g_ringSizeWords.load(), g_readPointer, Registers()[CP_RB_WPTR].get(), g_frames.load(), g_swapCounter.load());
    }
}
