// XMA decoding: the console's audio decoder hardware, for titles that drive its contexts themselves.
//
// A context is 64 bytes in physical memory: two input buffers of 2048 byte XMA packets, an output ring of 256 byte
// blocks receiving 16 bit big endian PCM (interleaved for stereo), and offsets and valid bits the title and the
// hardware hand back and forth. The title fills an input buffer and marks it valid, marks the output buffer valid and
// enables the context; the hardware decodes while there is input and room, advancing the read offset (in bits) and
// the write offset (in blocks), clearing an input buffer's valid bit when it has used it up and the output's when
// the ring is full.
//
// Frames are found here (each starts with its length in 15 bits and may cross packets and buffers) and decoded one
// at a time by FFmpeg's XMA2 decoder, wrapped in a packet of their own, so the read offset can be kept exact and
// loops and offsets set by the title work at frame granularity. Every frame gives 512 samples per channel; where the
// decoder gives none (its first frame after a reset), silence keeps the count. AR_NO_XMA leaves the decoder off
// (contexts then make no progress, as before), AR_LOG_XMA logs what contexts are given.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/memory.h>
#include <cpu/guest_thread.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

namespace
{
    constexpr uint32_t CONTEXT_SIZE = 64;
    constexpr uint32_t PACKET_BYTES = 2048;
    constexpr uint32_t PACKET_BITS = PACKET_BYTES * 8;
    constexpr uint32_t HEADER_BITS = 32;
    constexpr uint32_t BLOCK_BYTES = 256;
    constexpr uint32_t FRAME_SAMPLES = 512;
    constexpr uint32_t PADDING_LENGTH = 0x7FFF; // A frame length of all ones: no more frames in this packet.
    constexpr uint32_t SAMPLE_RATES[4] = { 24000, 32000, 44100, 48000 };

    // The context's words as the hardware lays them out (big endian, fields from the low bits up).
    struct ContextData
    {
        uint32_t w[10];

        uint32_t Get(int word, int shift, int bits) const { return (w[word] >> shift) & ((1u << bits) - 1); }
        void Set(int word, int shift, int bits, uint32_t value)
        {
            uint32_t mask = ((1u << bits) - 1) << shift;
            w[word] = (w[word] & ~mask) | ((value << shift) & mask);
        }

        // Word 0
        uint32_t InputPacketCount(int buffer) const { return buffer == 0 ? Get(0, 0, 12) : Get(1, 0, 12); }
        void SetInputPacketCount(int buffer, uint32_t value) { buffer == 0 ? Set(0, 0, 12, value) : Set(1, 0, 12, value); }
        uint32_t LoopCount() const { return Get(0, 12, 8); }
        void SetLoopCount(uint32_t value) { Set(0, 12, 8, value); }
        bool InputValid(int buffer) const { return Get(0, 20 + buffer, 1) != 0; }
        void SetInputValid(int buffer, bool valid) { Set(0, 20 + buffer, 1, valid); }
        uint32_t OutputBlockCount() const { return Get(0, 22, 5); }
        void SetOutputBlockCount(uint32_t value) { Set(0, 22, 5, value); }
        uint32_t OutputWriteOffset() const { return Get(0, 27, 5); }
        void SetOutputWriteOffset(uint32_t value) { Set(0, 27, 5, value); }
        // Word 1
        uint32_t LoopSubframeEnd() const { return Get(1, 14, 3); }
        uint32_t LoopSubframeSkip() const { return Get(1, 17, 3); }
        uint32_t SubframeDecodeCount() const { return Get(1, 20, 4); }
        uint32_t SampleRate() const { return Get(1, 27, 2); }
        bool Stereo() const { return Get(1, 29, 1) != 0; }
        bool OutputValid() const { return Get(1, 31, 1) != 0; }
        void SetOutputValid(bool valid) { Set(1, 31, 1, valid); }
        // Words 2 to 4
        uint32_t InputReadOffset() const { return Get(2, 0, 26); }
        void SetInputReadOffset(uint32_t value) { Set(2, 0, 26, value); }
        uint32_t LoopStart() const { return Get(3, 0, 26); }
        uint32_t LoopEnd() const { return Get(4, 0, 26); }
        uint32_t PacketMetadata() const { return Get(4, 26, 5); }
        int CurrentBuffer() const { return int(Get(4, 31, 1)); }
        void SetCurrentBuffer(int buffer) { Set(4, 31, 1, uint32_t(buffer)); }
        // Words 5 to 9
        uint32_t InputBuffer(int buffer) const { return buffer == 0 ? w[5] : w[6]; }
        void SetInputBuffer(int buffer, uint32_t physical) { w[buffer == 0 ? 5 : 6] = physical; }
        uint32_t OutputBuffer() const { return w[7]; }
        uint32_t OutputReadOffset() const { return Get(9, 0, 5); }
        void SetOutputReadOffset(uint32_t value) { Set(9, 0, 5, value); }
    };

    struct HostContext
    {
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        uint32_t channels = 0, sampleRate = 0;
        bool enabled = false;
        uint64_t framesDecoded = 0;
    };

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::map<uint32_t, HostContext> g_contexts; // By the context's guest (virtual) address.
    std::atomic<bool> g_kicked;

    const bool g_log = AR_ENV("AR_LOG_XMA") != nullptr;

    ContextData Load(uint32_t address)
    {
        ContextData data;
        auto* words = g_memory.Translate<be<uint32_t>>(address);
        for (int i = 0; i < 10; i++)
            data.w[i] = words[i];
        return data;
    }

    void Store(uint32_t address, const ContextData& data)
    {
        auto* words = g_memory.Translate<be<uint32_t>>(address);
        for (int i = 0; i < 10; i++)
            words[i] = data.w[i];
    }

    const uint8_t* PhysicalBytes(uint32_t physical)
    {
        return g_memory.Translate<const uint8_t>(PhysicalToVirtual(physical & 0x1FFFFFFF));
    }

    uint8_t* PhysicalBytesWritable(uint32_t physical)
    {
        return g_memory.Translate<uint8_t>(PhysicalToVirtual(physical & 0x1FFFFFFF));
    }

    uint32_t ReadBits(const uint8_t* bytes, uint32_t position, uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; i++, position++)
            value = (value << 1) | ((bytes[position >> 3] >> (7 - (position & 7))) & 1);
        return value;
    }

    void WriteBit(uint8_t* bytes, uint32_t position, uint32_t bit)
    {
        uint8_t mask = uint8_t(0x80 >> (position & 7));
        bytes[position >> 3] = bit ? (bytes[position >> 3] | mask) : (bytes[position >> 3] & ~mask);
    }

    void FreeCodec(HostContext& host)
    {
        avcodec_free_context(&host.codec);
        av_packet_free(&host.packet);
        av_frame_free(&host.frame);
        host.channels = host.sampleRate = 0;
    }

    bool OpenCodec(HostContext& host, uint32_t channels, uint32_t sampleRate)
    {
        if (host.codec != nullptr && host.channels == channels && host.sampleRate == sampleRate)
            return true;
        FreeCodec(host);

        const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (decoder == nullptr)
            return false;
        host.codec = avcodec_alloc_context3(decoder);
        host.codec->sample_rate = int(sampleRate);
        host.codec->block_align = PACKET_BYTES;
        av_channel_layout_default(&host.codec->ch_layout, int(channels));
        // The whole output, with nothing trimmed at the start: every frame is accounted for by the title.
        host.codec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        // XMA2WAVEFORMATEX: one stream, its channel mask.
        host.codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        host.codec->extradata_size = 34;
        host.codec->extradata[0] = 1;
        host.codec->extradata[2] = channels == 2 ? 0x03 : 0x04;
        if (avcodec_open2(host.codec, decoder, nullptr) < 0)
        {
            LOGF_ERROR("XMA: the decoder didn't open ({} channels, {} Hz)", channels, sampleRate);
            FreeCodec(host);
            return false;
        }
        host.packet = av_packet_alloc();
        host.frame = av_frame_alloc();
        host.channels = channels;
        host.sampleRate = sampleRate;
        return true;
    }

    // Where the input stands: the buffer being read and the bit in it.
    struct InputPosition
    {
        int buffer;
        uint32_t bit;
    };

    // The bits of one frame, gathered across packets and, if needed, into the other buffer.
    struct FrameBits
    {
        std::vector<uint8_t> bytes;
        uint32_t length = 0;
        InputPosition next{};
    };

    class Input
    {
    public:
        explicit Input(const ContextData& data) : m_data(data) {}

        bool Valid(int buffer) const { return m_data.InputValid(buffer) && m_data.InputBuffer(buffer) != 0 && m_data.InputPacketCount(buffer) != 0; }
        uint32_t EndBit(int buffer) const { return m_data.InputPacketCount(buffer) * PACKET_BITS; }
        const uint8_t* Bytes(int buffer) const { return PhysicalBytes(m_data.InputBuffer(buffer)); }

        // Moves to where the next frame starts, from a position that is at a frame or at a packet's payload. False
        // when the buffer has no more frames.
        bool FindFrame(InputPosition& at) const
        {
            for (int guard = 0; guard < 8192; guard++)
            {
                if (at.bit >= EndBit(at.buffer))
                    return false;
                uint32_t inPacket = at.bit % PACKET_BITS;
                if (inPacket < HEADER_BITS)
                {
                    at.bit += HEADER_BITS - inPacket;
                    inPacket = HEADER_BITS;
                }
                const uint8_t* bytes = Bytes(at.buffer);
                uint32_t packetStart = at.bit - inPacket;
                if (inPacket == HEADER_BITS)
                {
                    // At the start of a packet's payload: the bits before the first frame starting in it belong to a frame from the packet before.
                    uint32_t firstFrame = ReadBits(bytes, packetStart + 6, 15);
                    if (firstFrame >= PACKET_BITS - HEADER_BITS)
                    {
                        at.bit = NextPacket(at.buffer, packetStart);
                        continue;
                    }
                    at.bit += firstFrame;
                    inPacket += firstFrame;
                }
                if (PACKET_BITS - inPacket < 15 || ReadBits(bytes, at.bit, 15) == PADDING_LENGTH || ReadBits(bytes, at.bit, 15) == 0)
                {
                    at.bit = NextPacket(at.buffer, packetStart);
                    continue;
                }
                return true;
            }
            return false;
        }

        // The first payload bit of the packet after this one (packet skip counts are honoured for interleaved streams).
        uint32_t NextPacket(int buffer, uint32_t packetStart) const
        {
            uint32_t skip = ReadBits(Bytes(buffer), packetStart + 24, 8);
            if (skip == 0xFF)
                skip = 0;
            return packetStart + (1 + skip) * PACKET_BITS + HEADER_BITS;
        }

        // Reads the frame at a position FindFrame gave. False when it continues into input that isn't there yet.
        bool ReadFrame(InputPosition at, FrameBits& frame) const
        {
            const uint8_t* bytes = Bytes(at.buffer);
            frame.length = ReadBits(bytes, at.bit, 15);
            frame.bytes.assign((frame.length + 7) / 8, 0);
            int buffer = at.buffer;
            uint32_t bit = at.bit;
            for (uint32_t i = 0; i < frame.length; i++)
            {
                if (bit % PACKET_BITS == 0)
                    bit += HEADER_BITS;
                if (bit >= EndBit(buffer))
                {
                    // Into the other buffer, at its first packet's payload.
                    int other = buffer ^ 1;
                    if (buffer != at.buffer || !Valid(other))
                        return false;
                    buffer = other;
                    bytes = Bytes(buffer);
                    bit = HEADER_BITS;
                }
                WriteBit(frame.bytes.data(), i, ReadBits(bytes, bit, 1));
                bit++;
            }
            frame.next = { buffer, bit };
            return true;
        }

    private:
        const ContextData& m_data;
    };

    // Decodes one frame into 16 bit big endian PCM. Silence when the decoder gives nothing for it.
    void DecodeFrame(HostContext& host, const FrameBits& frame, std::vector<uint8_t>& pcm)
    {
        uint32_t channels = host.channels;
        pcm.assign(FRAME_SAMPLES * channels * 2, 0);

        // A packet holding only this frame: one frame, starting right after the header, then padding.
        if (frame.length > PACKET_BITS - HEADER_BITS)
            return;
        if (av_new_packet(host.packet, PACKET_BYTES) < 0)
            return;
        uint8_t* data = host.packet->data;
        memset(data, 0xFF, PACKET_BYTES);
        data[0] = 1 << 2; // 6 bits frame count (1), 15 bits offset of the first frame (0), 3 bits metadata, 8 bits skip (0).
        data[1] = 0;
        data[2] = 0;
        data[3] = 0;
        for (uint32_t i = 0; i < frame.length; i++)
            WriteBit(data, HEADER_BITS + i, (frame.bytes[i >> 3] >> (7 - (i & 7))) & 1);

        if (avcodec_send_packet(host.codec, host.packet) < 0)
        {
            av_packet_unref(host.packet);
            return;
        }
        av_packet_unref(host.packet);

        uint32_t written = 0;
        while (avcodec_receive_frame(host.codec, host.frame) == 0)
        {
            uint32_t samples = std::min<uint32_t>(uint32_t(host.frame->nb_samples), FRAME_SAMPLES - written);
            int frameChannels = host.frame->ch_layout.nb_channels;
            for (uint32_t s = 0; s < samples; s++)
            {
                for (uint32_t c = 0; c < channels; c++)
                {
                    const float* plane = reinterpret_cast<const float*>(host.frame->extended_data[std::min<int>(int(c), frameChannels - 1)]);
                    float value = std::clamp(plane[s], -1.0f, 1.0f);
                    int16_t sample = int16_t(std::lrintf(value * 32767.0f));
                    size_t index = ((written + s) * channels + c) * 2;
                    pcm[index] = uint8_t(uint16_t(sample) >> 8);
                    pcm[index + 1] = uint8_t(sample);
                }
            }
            written += samples;
            av_frame_unref(host.frame);
        }
    }

    // What the hardware does for one context: decode while there is input and room for a whole frame.
    void Decode(uint32_t address, HostContext& host)
    {
        ContextData data = Load(address);
        if (!data.OutputValid() || data.OutputBuffer() == 0)
            return;

        uint32_t channels = data.Stereo() ? 2 : 1;
        if (!OpenCodec(host, channels, SAMPLE_RATES[data.SampleRate()]))
            return;

        Input input(data);
        if (!input.Valid(0) && !input.Valid(1))
        {
            // Nothing to decode: the hardware gives the output buffer back.
            data.SetOutputValid(false);
            Store(address, data);
            return;
        }

        uint32_t blockCount = data.OutputBlockCount() != 0 ? data.OutputBlockCount() : 32;
        uint32_t capacity = blockCount * BLOCK_BYTES;
        uint32_t readOffset = (data.OutputReadOffset() % blockCount) * BLOCK_BYTES;
        uint32_t writeOffset = (data.OutputWriteOffset() % blockCount) * BLOCK_BYTES;
        uint32_t frameBytes = FRAME_SAMPLES * channels * 2;
        uint8_t* output = PhysicalBytesWritable(data.OutputBuffer());
        // An empty ring has the offsets equal; it's full when they are equal again after writing (then the output stops being valid).
        uint32_t room = readOffset == writeOffset ? capacity : (readOffset + capacity - writeOffset) % capacity;

        bool changed = false;
        std::vector<uint8_t> pcm;
        FrameBits frame;
        while (room >= frameBytes)
        {
            int buffer = data.CurrentBuffer();
            if (!input.Valid(buffer))
            {
                if (!input.Valid(buffer ^ 1))
                    break;
                buffer ^= 1;
                data.SetCurrentBuffer(buffer);
                data.SetInputReadOffset(HEADER_BITS);
                changed = true;
            }

            InputPosition at{ buffer, std::max(data.InputReadOffset(), HEADER_BITS) };
            if (!input.FindFrame(at))
            {
                // This buffer is used up: give it back and go on with the other.
                data.SetInputValid(buffer, false);
                data.SetCurrentBuffer(buffer ^ 1);
                data.SetInputReadOffset(HEADER_BITS);
                changed = true;
                continue;
            }
            if (!input.ReadFrame(at, frame))
            {
                // The frame goes on in input not given yet.
                data.SetInputReadOffset(at.bit);
                changed = true;
                break;
            }

            DecodeFrame(host, frame, pcm);
            host.framesDecoded++;
            for (uint32_t done = 0; done < frameBytes;)
            {
                uint32_t part = std::min(frameBytes - done, capacity - writeOffset);
                memcpy(output + writeOffset, pcm.data() + done, part);
                done += part;
                writeOffset = (writeOffset + part) % capacity;
            }
            room -= frameBytes;
            changed = true;

            // The frame came partly from the other buffer: this one is used up.
            if (frame.next.buffer != buffer)
            {
                data.SetInputValid(buffer, false);
                data.SetCurrentBuffer(frame.next.buffer);
            }
            uint32_t next = frame.next.bit;
            // Loops, at frame granularity: from the end back to the start, the given number of times (255 forever).
            if (data.LoopCount() != 0 && data.LoopEnd() != 0 && next >= data.LoopEnd() && frame.next.buffer == buffer)
            {
                next = data.LoopStart();
                if (data.LoopCount() != 255)
                    data.SetLoopCount(data.LoopCount() - 1);
            }
            data.SetInputReadOffset(next);
        }

        if (!changed)
            return;
        data.SetOutputWriteOffset(writeOffset / BLOCK_BYTES);
        if (writeOffset == readOffset)
            data.SetOutputValid(false);
        Store(address, data);
    }

    void RunDecoder()
    {
        std::unique_lock lock(g_mutex);
        while (true)
        {
            g_wake.wait_for(lock, std::chrono::milliseconds(2), [] { return g_kicked.load(); });
            g_kicked = false;
            for (auto& [address, host] : g_contexts)
            {
                if (host.enabled)
                    Decode(address, host);
            }
        }
    }

    void Kick()
    {
        g_kicked = true;
        g_wake.notify_one();
    }

    void StartDecoder()
    {
        static std::once_flag once;
        std::call_once(once, []
            {
                if (AR_ENV("AR_NO_XMA"))
                {
                    LOGF_WARNING("XMA decoding is off (AR_NO_XMA): sounds using it are silent");
                    return;
                }
                ThreadStartParams params{};
                params.hostEntry = RunDecoder;
                params.name = "XMA decoder";
                CreateGuestThread(params);
                LOGF_INFO("XMA decoder started");
            });
    }

    // Applies a change to a context's words under the decoder's lock.
    template <typename F>
    auto Change(uint32_t context, F&& change)
    {
        std::lock_guard lock(g_mutex);
        ContextData data = Load(context);
        if constexpr (std::is_void_v<decltype(change(data))>)
        {
            change(data);
            Store(context, data);
            Kick();
        }
        else
        {
            auto result = change(data);
            Store(context, data);
            Kick();
            return result;
        }
    }

    template <typename F>
    auto Read(uint32_t context, F&& read)
    {
        std::lock_guard lock(g_mutex);
        return read(Load(context));
    }
}

// XMA_CONTEXT_INIT and XMA_LOOP_DATA, as titles pass them.
struct XmaLoopData
{
    be<uint32_t> loopStart;
    be<uint32_t> loopEnd;
    uint8_t loopCount;
    uint8_t loopSubframeEnd;
    uint8_t loopSubframeSkip;
    uint8_t padding;
};

struct XmaContextInit
{
    be<uint32_t> inputBuffer0;
    be<uint32_t> inputBuffer0PacketCount;
    be<uint32_t> inputBuffer1;
    be<uint32_t> inputBuffer1PacketCount;
    be<uint32_t> inputBufferReadOffset;
    be<uint32_t> outputBuffer;
    be<uint32_t> outputBufferBlockCount;
    be<uint32_t> workBuffer;
    be<uint32_t> subframeDecodeCount;
    be<uint32_t> channelCount;
    be<uint32_t> sampleRate;
    XmaLoopData loopData;
};

static void ApplyLoopData(ContextData& data, const XmaLoopData* loop)
{
    data.Set(3, 0, 26, loop->loopStart);
    data.Set(4, 0, 26, loop->loopEnd);
    data.SetLoopCount(loop->loopCount);
    data.Set(1, 14, 3, loop->loopSubframeEnd);
    data.Set(1, 17, 3, loop->loopSubframeSkip);
}

namespace imports
{
    // XMA contexts live in physical memory, where the decoder hardware reads them.
    uint32_t XMACreateContext(be<uint32_t>* context)
    {
        uint32_t address = AllocatePhysicalMemory(CONTEXT_SIZE, 0x1000, PHYSICAL_VIEW_4K, 0x04, false);
        *context = address;
        if (address == 0)
            return X_STATUS_NO_MEMORY;
        memset(g_memory.Translate(address), 0, CONTEXT_SIZE);
        {
            std::lock_guard lock(g_mutex);
            g_contexts[address] = {};
        }
        StartDecoder();
        return X_STATUS_SUCCESS;
    }

    void XMAReleaseContext(uint32_t context)
    {
        {
            std::lock_guard lock(g_mutex);
            auto it = g_contexts.find(context);
            if (it != g_contexts.end())
            {
                FreeCodec(it->second);
                g_contexts.erase(it);
            }
        }
        FreePhysicalMemory(context);
    }

    uint32_t XMAInitializeContext(uint32_t context, XmaContextInit* init)
    {
        std::lock_guard lock(g_mutex);
        ContextData data{};
        data.SetInputBuffer(0, init->inputBuffer0 != 0 ? VirtualToPhysical(init->inputBuffer0) : 0);
        data.SetInputPacketCount(0, init->inputBuffer0PacketCount);
        data.SetInputValid(0, init->inputBuffer0 != 0);
        data.SetInputBuffer(1, init->inputBuffer1 != 0 ? VirtualToPhysical(init->inputBuffer1) : 0);
        data.SetInputPacketCount(1, init->inputBuffer1PacketCount);
        data.SetInputValid(1, init->inputBuffer1 != 0);
        data.SetInputReadOffset(init->inputBufferReadOffset);
        data.w[7] = init->outputBuffer != 0 ? VirtualToPhysical(init->outputBuffer) : 0;
        data.SetOutputBlockCount(init->outputBufferBlockCount);
        data.w[8] = init->workBuffer != 0 ? VirtualToPhysical(init->workBuffer) : 0;
        data.Set(1, 20, 4, init->subframeDecodeCount);
        data.Set(1, 29, 1, init->channelCount >= 1 ? 1 : 0);
        data.Set(1, 27, 2, std::min<uint32_t>(init->sampleRate, 3));
        ApplyLoopData(data, &init->loopData);
        Store(context, data);

        auto it = g_contexts.find(context);
        if (it != g_contexts.end())
        {
            // A new sound: nothing carries over from the last one.
            if (it->second.codec != nullptr)
                avcodec_flush_buffers(it->second.codec);
        }
        if (g_log)
            LOGF_INFO("XMA {:08X}: init, input {:08X} ({} packets) {:08X} ({}), read {}, output {:08X} ({} blocks), {} channel(s) at {} Hz, loop {}-{} x{}",
                context, data.InputBuffer(0), data.InputPacketCount(0), data.InputBuffer(1), data.InputPacketCount(1), data.InputReadOffset(),
                data.OutputBuffer(), data.OutputBlockCount(), data.Stereo() ? 2 : 1, SAMPLE_RATES[data.SampleRate()], data.LoopStart(), data.LoopEnd(), data.LoopCount());
        Kick();
        return X_STATUS_SUCCESS;
    }

    uint32_t XMASetLoopData(uint32_t context, XmaLoopData* loop)
    {
        Change(context, [&](ContextData& data) { ApplyLoopData(data, loop); });
        return X_STATUS_SUCCESS;
    }

    uint32_t XMAGetInputBufferReadOffset(uint32_t context)
    {
        return Read(context, [](const ContextData& data) { return data.InputReadOffset(); });
    }

    uint32_t XMASetInputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        Change(context, [&](ContextData& data) { data.SetInputReadOffset(offset); });
        return X_STATUS_SUCCESS;
    }

    static uint32_t SetInputBuffer(uint32_t context, int buffer, uint32_t address, uint32_t packets)
    {
        Change(context, [&](ContextData& data)
            {
                data.SetInputBuffer(buffer, address != 0 ? VirtualToPhysical(address) : 0);
                data.SetInputPacketCount(buffer, packets);
            });
        if (g_log)
            LOGF_INFO("XMA {:08X}: input {} = {:08X}, {} packets", context, buffer, address, packets);
        return X_STATUS_SUCCESS;
    }

    uint32_t XMASetInputBuffer0(uint32_t context, uint32_t address, uint32_t packets) { return SetInputBuffer(context, 0, address, packets); }
    uint32_t XMASetInputBuffer1(uint32_t context, uint32_t address, uint32_t packets) { return SetInputBuffer(context, 1, address, packets); }

    uint32_t XMAIsInputBuffer0Valid(uint32_t context) { return Read(context, [](const ContextData& data) { return uint32_t(data.InputValid(0)); }); }
    uint32_t XMAIsInputBuffer1Valid(uint32_t context) { return Read(context, [](const ContextData& data) { return uint32_t(data.InputValid(1)); }); }

    uint32_t XMASetInputBuffer0Valid(uint32_t context)
    {
        Change(context, [](ContextData& data) { data.SetInputValid(0, true); });
        return X_STATUS_SUCCESS;
    }

    uint32_t XMASetInputBuffer1Valid(uint32_t context)
    {
        Change(context, [](ContextData& data) { data.SetInputValid(1, true); });
        return X_STATUS_SUCCESS;
    }

    uint32_t XMAIsOutputBufferValid(uint32_t context) { return Read(context, [](const ContextData& data) { return uint32_t(data.OutputValid()); }); }

    uint32_t XMASetOutputBufferValid(uint32_t context)
    {
        Change(context, [](ContextData& data) { data.SetOutputValid(true); });
        return X_STATUS_SUCCESS;
    }

    uint32_t XMAGetOutputBufferReadOffset(uint32_t context) { return Read(context, [](const ContextData& data) { return data.OutputReadOffset(); }); }

    uint32_t XMASetOutputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        Change(context, [&](ContextData& data) { data.SetOutputReadOffset(offset); });
        return X_STATUS_SUCCESS;
    }

    uint32_t XMAGetOutputBufferWriteOffset(uint32_t context) { return Read(context, [](const ContextData& data) { return data.OutputWriteOffset(); }); }

    uint32_t XMAGetPacketMetadata(uint32_t context) { return Read(context, [](const ContextData& data) { return data.PacketMetadata(); }); }

    uint32_t XMAEnableContext(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_contexts.find(context);
        if (it != g_contexts.end())
            it->second.enabled = true;
        Kick();
        return X_STATUS_SUCCESS;
    }

    // The decoder works under the same lock, so once this returns it isn't using the context.
    uint32_t XMADisableContext(uint32_t context, uint32_t wait)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_contexts.find(context);
        if (it != g_contexts.end())
            it->second.enabled = false;
        return X_STATUS_SUCCESS;
    }

    uint32_t XMABlockWhileInUse(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        return X_STATUS_SUCCESS;
    }
}

AR_IMPORT(XMACreateContext);
AR_IMPORT(XMAReleaseContext);
AR_IMPORT(XMAInitializeContext);
AR_IMPORT(XMASetLoopData);
AR_IMPORT(XMAGetInputBufferReadOffset);
AR_IMPORT(XMASetInputBufferReadOffset);
AR_IMPORT(XMASetInputBuffer0);
AR_IMPORT(XMASetInputBuffer1);
AR_IMPORT(XMAIsInputBuffer0Valid);
AR_IMPORT(XMAIsInputBuffer1Valid);
AR_IMPORT(XMASetInputBuffer0Valid);
AR_IMPORT(XMASetInputBuffer1Valid);
AR_IMPORT(XMAIsOutputBufferValid);
AR_IMPORT(XMASetOutputBufferValid);
AR_IMPORT(XMAGetOutputBufferReadOffset);
AR_IMPORT(XMASetOutputBufferReadOffset);
AR_IMPORT(XMAGetOutputBufferWriteOffset);
AR_IMPORT(XMAGetPacketMetadata);
AR_IMPORT(XMAEnableContext);
AR_IMPORT(XMADisableContext);
AR_IMPORT(XMABlockWhileInUse);
