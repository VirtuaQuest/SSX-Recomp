#include <stdafx.h>
#include "command_processor.h"
#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include "registers.h"
#include "video_settings.h"
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
        PM4_EVENT_WRITE_CFL = 0x59,
        PM4_EVENT_WRITE_EXT = 0x5A,
        PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_SET_BIN_MASK = 0x50,
        PM4_SET_BIN_SELECT = 0x51,
        PM4_SET_CONSTANT2 = 0x55,
        PM4_SET_BIN_MASK_LO = 0x60,
        PM4_SET_BIN_MASK_HI = 0x61,
        PM4_SET_BIN_SELECT_LO = 0x62,
        PM4_SET_BIN_SELECT_HI = 0x63,
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

    // When VdSwap was called for each swap the GPU hasn't reached yet, to measure how far behind it runs.
    static std::mutex g_swapTimesMutex;
    static std::deque<std::chrono::steady_clock::time_point> g_swapTimes;

    void NoteSwapSubmitted()
    {
        std::lock_guard lock(g_swapTimesMutex);
        g_swapTimes.push_back(std::chrono::steady_clock::now());

        static uint64_t calls;
        static auto lastReport = std::chrono::steady_clock::now();
        calls++;
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(5))
        {
            LOGF_INFO("VdSwap: {} calls in {:.1f} s, GPU swaps {}", calls,
                std::chrono::duration<double>(now - lastReport).count(), g_frames.load());
            calls = 0;
            lastReport = now;
        }
    }

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
        // The game presents every second vblank (30 fps at 60 Hz); a higher frame rate setting raises the rate.
        const int64_t interval = 1000000 / (int64_t(g_videoSettings.frameRate) * 2);
        auto next = std::chrono::steady_clock::now();
        while (true)
        {
            next += std::chrono::microseconds(interval);
            std::this_thread::sleep_until(next);

            g_swapCounter++;

            if (AR_ENV("AR_LOG_GPU_WAITS"))
            {
                static int logged;
                uint32_t value = *g_memory.Translate<be<uint32_t>>(PhysicalToVirtual(0x0CCD7004));
                if (logged++ < 40)
                    LOGF_INFO("vblank {}: flag 0x{:08X}", g_swapCounter.load(), value);
            }

            // The scanline counter advances by a frame's worth of lines, so code timing frames with it sees time pass.
            auto& counter = Registers()[0x194C]; // D1MODE_V_COUNTER
            counter = counter + 720;

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
        uint32_t Size() const { return m_size; }
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

    static void PublishReadPointer(uint32_t position)
    {
        g_readPointer = position;
        Registers()[CP_RB_RPTR] = position;
        if (uint32_t writeBack = g_readPointerWriteBack)
            *g_memory.Translate<be<uint32_t>>(PhysicalToVirtual(writeBack)) = position;
    }

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

    // Predicated tiling: a frame is rendered in screen tiles ("bins") by running the same commands once per tile.
    // Type 3 packets with the predicate bit only run when the selected bins overlap the mask, so per frame work
    // (fences, swaps) inside them runs once.
    static uint64_t g_binMask = 0xFFFFFFFF;
    static uint64_t g_binSelect = 0xFFFFFFFF;

    static void ExecutePacket3(PacketReader& reader, uint32_t opcode, uint32_t count, bool predicated)
    {
        uint32_t start = reader.Position();
        uint32_t consumed = 0;
        auto read = [&]() { consumed++; return reader.Read(); };

        // Predicated swaps are skipped too: a frame is presented once, not per tile.
        if (predicated && ((g_binSelect & g_binMask) == 0 || opcode == PM4_XE_SWAP))
        {
            reader.Skip(count);
            return;
        }

        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t address = read() & ~3u;
            uint32_t length = read() & 0xFFFFF;
            LOGF_TRACE("indirect buffer 0x{:08X}, {} words", address, length);
            if (AR_ENV("AR_LOG_IBS") && g_frames >= 20 && g_frames < 24)
                LOGF_INFO("frame {} depth {}: indirect buffer 0x{:08X}, {} words (ring position {})", g_frames.load(), g_indirectDepth,
                    address, length, reader.Position());

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

        case PM4_SET_BIN_MASK:
            g_binMask = uint64_t(read()) << 32;
            g_binMask |= read();
            break;

        case PM4_SET_BIN_SELECT:
            g_binSelect = uint64_t(read()) << 32;
            g_binSelect |= read();
            break;

        case PM4_SET_BIN_MASK_LO:
            g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | read();
            break;

        case PM4_SET_BIN_MASK_HI:
            g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(read()) << 32);
            break;

        case PM4_SET_BIN_SELECT_LO:
            g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | read();
            break;

        case PM4_SET_BIN_SELECT_HI:
            g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(read()) << 32);
            break;

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
            if (AR_ENV("AR_LOG_GPU_WAITS") && (address & ~3u) == 0x0CCD7004)
            {
                static int logged;
                if (logged++ < 20)
                    LOGF_INFO("GPU writes 0x{:08X} words to 0x{:08X}, first 0x{:X}", count - 1, address, reader.Position());
            }
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
                std::this_thread::yield();
                if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
                {
                    LOGF_WARNING("GPU is waiting on {} 0x{:08X} (function {}, reference 0x{:X}, mask 0x{:X})",
                        (waitInfo & 0x10) ? "memory" : "register", pollAddress, waitInfo & 7, reference, mask);
                    warned = true;
                }
            }

            if (AR_ENV("AR_LOG_GPU_WAITS"))
            {
                double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                static int logged;
                if (waited > 1.0 && logged++ < 60)
                    LOGF_INFO("GPU waited {:.1f} ms on {} 0x{:08X} (function {}, reference 0x{:X}, mask 0x{:X}), value now 0x{:X}", waited,
                        (waitInfo & 0x10) ? "memory" : "register", pollAddress, waitInfo & 7, reference, mask, ReadPollValue(waitInfo, pollAddress));
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

            if (AR_ENV("AR_LOG_IBS") && g_frames >= 20 && g_frames < 24)
                LOGF_INFO("frame {}: fence write {} to 0x{:08X}", g_frames.load(), value, address);
            if (AR_ENV("AR_LOG_FENCES"))
            {
                static std::map<uint32_t, uint32_t> last;
                static int logged;
                uint32_t& previous = last[address];
                if (value < previous && logged++ < 30)
                    LOGF_INFO("fence at 0x{:08X} went back from {} to {} (frame {})", address, previous, value, g_frames.load());
                if (address == 0x0CCD4000 && logged++ < 30)
                    LOGF_INFO("fence 0x{:08X} = {} (frame {})", address, value, g_frames.load());
                previous = value;
            }
            LOGF_TRACE("event write 0x{:08X} to 0x{:08X} (initiator 0x{:08X})", value, address, initiator);
            break;
        }

        case PM4_EVENT_WRITE_EXT:
        {
            uint32_t initiator = read();
            uint32_t address = read();
            WriteRegister(VGT_EVENT_INITIATOR, initiator & 0x3F);

            // Screen extents of the preceding draws (min/max x, y in units of 8 pixels, then z), which D3D uses to
            // decide which tiles of a predicated tiling frame each object touches. Not measured: the maximum surface
            // size is reported, so objects draw in every tile, as Xenia does.
            static const uint16_t extents[] = { 0, 8192 / 8, 0, 8192 / 8, 0, 1 };
            auto* out = g_memory.Translate<be<uint16_t>>(PhysicalToVirtual(address & ~3u));
            for (size_t i = 0; i < std::size(extents); i++)
                out[i] = extents[i];
            break;
        }

        case PM4_EVENT_WRITE_ZPD:
        {
            // Occlusion query begin and end both write a report of cumulative sample counts to RB_SAMPLE_COUNT_ADDR;
            // D3D subtracts them and polls until the end report replaces its pending marker. Queries aren't
            // measured: the counter advances by a fixed amount, so every query completes and reports visible.
            WriteRegister(VGT_EVENT_INITIATOR, read() & 0x3F);
            static uint64_t samples;
            samples += 4096;
            uint32_t address = Registers()[RB_SAMPLE_COUNT_ADDR];
            if (address != 0)
            {
                auto* report = g_memory.Translate<uint32_t>(PhysicalToVirtual(address & ~3u));
                uint32_t a = uint32_t(samples) - (uint32_t(samples) >> 1), b = uint32_t(samples) >> 1;
                const uint32_t values[8] = { a, b, 0, 0, a, b, 0, 0 }; // Total, ZFail, ZPass, StencilFail (little endian).
                memcpy(report, values, sizeof(values));
            }
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
            {
                std::lock_guard lock(g_swapTimesMutex);
                LOGF_INFO("frame {} ({} swaps submitted but not reached)", frame, g_swapTimes.size());
            }

            // Written by VdSwap: 'SWAP', the front buffer's physical address and size, then its fetch constant.
            SwapInfo swap{};
            read();
            swap.frontBuffer = read();
            swap.width = read();
            swap.height = read();
            for (auto& word : swap.fetchConstant)
                word = read();
            {
                std::lock_guard lock(g_swapTimesMutex);
                if (!g_swapTimes.empty())
                {
                    auto submitted = g_swapTimes.front();
                    g_swapTimes.pop_front();
                    if (AR_ENV("AR_LOG_SWAP_LATENCY") != nullptr && (frame % 30) == 0)
                        LOGF_INFO("frame {}: swap latency {:.1f} ms, {} swaps queued", frame,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitted).count(), g_swapTimes.size());
                }
            }
            Swap(Registers(), swap);
            break;
        }

        default:
        {
            static std::set<uint32_t> logged;
            static std::mutex mutex;
            std::lock_guard lock(mutex);
            if (logged.insert(opcode).second)
                LOGF_WARNING("unhandled PM4 packet 0x{:02X} ({} words)", opcode, count);
            break;
        }
        }

        // Packets carry their length, so anything not consumed by the handler (or not handled) is skipped.
        if (consumed < count)
            reader.Skip(count - consumed);
        else if (consumed > count)
        {
            static std::set<uint32_t> logged;
            if (logged.insert(opcode).second)
                LOGF_ERROR("PM4 packet 0x{:02X} handler read {} words of a {} word packet", opcode, consumed, count);
        }
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
            ExecutePacket3(reader, (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1, header & 1);
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

        // The ring's read pointer advances packet by packet, like the hardware's fetcher: D3D waits on it for ring
        // space, and holding it back while a packet blocks (a swap waits up to two vertical blanks) stalls
        // the title's render thread, and with it everything waiting on GPU fences.
        while (reader.Position() != endPosition)
        {
            uint32_t before = reader.Position();
            uint32_t remaining = (endPosition + reader.Size() - before) % reader.Size();
            ExecutePacket(reader);

            // A packet extending past the write pointer would make the loop run around the whole ring, replaying
            // stale commands. Stop at the write pointer instead.
            uint32_t advanced = (reader.Position() + reader.Size() - before) % reader.Size();
            if (advanced > remaining)
            {
                LOGF_ERROR("PM4 packet at {} extends past the write pointer {} (advanced {} words)", before, endPosition, advanced);
                PublishReadPointer(endPosition);
                return;
            }
            PublishReadPointer(reader.Position());
        }
    }

    static void RunCommandProcessor()
    {
        auto lastWork = std::chrono::steady_clock::now();
        while (true)
        {
            uint32_t base = g_ringBase;
            uint32_t size = g_ringSizeWords;
            uint32_t writePointer = Registers()[CP_RB_WPTR];

            if (base == 0 || size == 0 || writePointer == g_readPointer || writePointer >= size)
            {
                // The CPU often waits on the GPU right after submitting (fences, occlusion queries), and Windows
                // sleeps last a millisecond or more, so new work is polled for without sleeping for a while.
                if (std::chrono::steady_clock::now() - lastWork < std::chrono::milliseconds(50))
                    std::this_thread::yield();
                else
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }

            lastWork = std::chrono::steady_clock::now();
            if (AR_ENV("AR_LOG_FENCES") && writePointer < g_readPointer)
            {
                static int logged;
                if (logged++ < 30)
                    LOGF_INFO("ring wraps: read pointer {}, write pointer {} ({} words of {})", g_readPointer, writePointer,
                        writePointer + size - g_readPointer, size);
            }

            PacketReader reader(base, size, g_readPointer, true);
            ExecuteBuffer(reader, writePointer);
            PublishReadPointer(writePointer);
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
