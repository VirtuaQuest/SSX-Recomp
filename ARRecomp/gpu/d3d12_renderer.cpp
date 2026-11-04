#include <stdafx.h>
#include <array>
#include "d3d12_renderer.h"
#include "shader_compiler.h"
#include "texture_decode.h"
#include "window.h"
#include "video_settings.h"
#include "deferred_list.h"
#include "command_processor.h"
#include <kernel/keyboard_mouse.h>
#include <kernel/memory.h>
#include "xenia/gpu/shader.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace gpu::d3d12
{
    using namespace reg;

    constexpr uint32_t FRAME_COUNT = 2;
    // Experiment (AR_UPLOAD_RING): 1 GB of upload space, each frame continuing where its slot's last frame stopped,
    // so upload addresses come back only every few frames.
    static const bool g_uploadRing = getenv("AR_UPLOAD_RING") != nullptr;
    static const uint64_t UPLOAD_SIZE = g_uploadRing ? 1024ull << 20 : 256ull << 20;
    // Vertex buffers that don't change are kept past the per-frame upload space, in the same buffer.
    static const uint64_t VERTEX_CACHE_BASE = UPLOAD_SIZE + 4096;
    constexpr uint64_t VERTEX_CACHE_SIZE = 384ull << 20;
    constexpr uint32_t VIEW_HEAP_SIZE = 65536;
    constexpr uint32_t SAMPLER_HEAP_SIZE = 2048;
    constexpr uint32_t RTV_HEAP_SIZE = 1024;
    constexpr uint32_t DSV_HEAP_SIZE = 256;
    constexpr uint32_t RENDER_TARGET_HEIGHT = 2048;

    // Internal resolution scale (g_videoSettings.renderScale): render targets and resolved textures are this many
    // times larger in each direction. EDRAM bookkeeping (rectangles, rows) stays in guest samples, scaled when the
    // GPU is told about it.
    static uint32_t g_scale = 1;

    static D3D12_RECT Scaled(D3D12_RECT rect)
    {
        return { LONG(rect.left * g_scale), LONG(rect.top * g_scale), LONG(rect.right * g_scale), LONG(rect.bottom * g_scale) };
    }
    constexpr uint32_t NULL_SRV_2D = 0, NULL_SRV_3D = 1, NULL_SRV_CUBE = 2;

    // Must match XeSystemConstants in hlsl_translator.cpp.
    struct SystemConstants
    {
        uint32_t boolConstants[8];
        uint32_t loopConstants[32];
        uint32_t fetchConstants[192];
        float ndcScale[4];
        float ndcOffset[4];
        uint32_t flags;
        int32_t vertexIndexOffset;
        int32_t paramGenRegister;
        float alphaRef;
        uint32_t alphaFunc;
        uint32_t gammaTargets;
        float resolutionScale;
        float padding;
        float colorExpBias[4];
        uint32_t textures[32][4];
        int32_t vertexDataOffsets[96];
        float textureTransforms[32][4]; // UV scale and offset, for textures inside larger resolved textures.
        float point[4]; // Constant diameter x, y, per-vertex diameter minimum, maximum.
        float pointNdc[4]; // NDC radius per pixel of diameter x, y.
    };
    static_assert(sizeof(SystemConstants) == 2448);

    enum RootParameter : uint32_t
    {
        ROOT_SYSTEM_CONSTANTS,
        ROOT_VERTEX_FLOAT_CONSTANTS,
        ROOT_PIXEL_FLOAT_CONSTANTS,
        ROOT_VERTEX_DATA,
        ROOT_TEXTURES_2D,
        ROOT_TEXTURES_3D,
        ROOT_TEXTURES_CUBE,
        ROOT_SAMPLERS,
        ROOT_TEXTURES_UINT, // Integer views (depth buffers' stencil), for depth resolves.
        ROOT_PARAMETER_COUNT,
    };

    struct Texture
    {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t srv = UINT32_MAX;
        uint32_t stencilSrv = UINT32_MAX; // Depth buffers.
        uint32_t rtv = UINT32_MAX;
        uint32_t dsv = UINT32_MAX;
        // Render targets: the sample layout their texels are in. 0 is EDRAM order (as single sampled surfaces see
        // it); 1 and 2 are as rasterized with 2x and 4x MSAA (a sample per texel, samples of a pixel adjacent).
        uint32_t layout = 0;
        uint64_t poolKey = 0; // Its description, for reuse once released (0: not pooled).
    };

    struct DescriptorAllocator
    {
        std::vector<uint32_t> freeList;
        uint32_t next = 0;
        uint32_t capacity = 0;

        uint32_t Allocate()
        {
            if (!freeList.empty())
            {
                uint32_t index = freeList.back();
                freeList.pop_back();
                return index;
            }
            return next < capacity ? next++ : UINT32_MAX;
        }

        void Free(uint32_t index)
        {
            if (index != UINT32_MAX)
                freeList.push_back(index);
        }
    };

    struct UploadAllocation
    {
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
        uint64_t offset; // From the start of the upload buffer.
    };

    struct FrameResources
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
        std::vector<ComPtr<ID3D12Resource>> releasedResources;
        std::vector<uint32_t> releasedSrvs;
        // Render target and depth views are read when commands are replayed, so they are reused after the frame too.
        std::vector<uint32_t> releasedRtvs;
        std::vector<uint32_t> releasedDsvs;
        // Released textures, reusable (by CreateTexture) once the GPU is done with this frame.
        std::vector<Texture> pooledTextures;
        // Diagnostics (AR_CHECK_UPLOADS): upload memory the frame's draws read, checked once the GPU is done.
        struct UploadCheck
        {
            uint64_t offset, size, hash, frame;
            const char* what;
        };
        std::vector<UploadCheck> uploadChecks;
    };

    struct RenderTargetKey
    {
        uint32_t base;
        uint32_t pitch;
        uint32_t format;
        bool depth;

        bool operator==(const RenderTargetKey&) const = default;
    };

    struct RenderTargetKeyHash
    {
        size_t operator()(const RenderTargetKey& key) const
        {
            return size_t(key.base) ^ (size_t(key.pitch) << 13) ^ (size_t(key.format) << 28) ^ (size_t(key.depth) << 33);
        }
    };

    // A texture written by resolves. Its guest memory spans rowBytes * height bytes from its address (tiled
    // layouts keep whole rows of 32 contiguous, and resolves start at multiples of 8 rows), and resolves and
    // fetches can address it at any row.
    struct ResolvedTexture
    {
        Texture texture;
        uint32_t format;
        uint32_t rowBytes;
        bool rawDepth = false; // Holds depth buffer words (from depth resolves), 8 bit stencil in the low byte.
        uint64_t version = 0;  // Changes with every resolve into it.
    };

    // Resolved textures that follow each other in memory, fetched as one texture (shadow map cascades resolved one
    // below the other and sampled as one tall atlas), copied together. Rebuilt when a part is resolved again.
    struct CompositeTexture
    {
        Texture texture;
        std::vector<std::pair<uint32_t, uint64_t>> parts; // Address and version of each part copied in.
    };

    // Detects changes of guest memory a cached copy was made from. Small ranges are hashed whole every check. Large
    // ones that have stayed the same for a while are checked with a sampled hash (spread over the range, so most
    // writes show), and hashed whole every 30 frames.
    struct ContentCheck
    {
        uint64_t hash = 0;
        uint64_t sampleHash = 0;
        uint32_t stableChecks = 0;
        uint32_t phase = 0; // Spreads the full checks of different ranges over frames.
    };

    static uint64_t HashMemory(const uint8_t* data, size_t size);

    static uint64_t SampleHash(const uint8_t* data, size_t size)
    {
        constexpr size_t SAMPLES = 64, CHUNK = 64;
        // Four independent lanes, so the multiplications overlap.
        uint64_t lanes[4] = { size, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull };
        size_t step = (size - CHUNK) / (SAMPLES - 1);
        for (size_t i = 0; i < SAMPLES; i++)
        {
            const uint8_t* chunk = data + std::min(i * step, size - CHUNK);
            for (size_t j = 0; j < CHUNK; j += 32)
            {
                for (int lane = 0; lane < 4; lane++)
                {
                    uint64_t value;
                    memcpy(&value, chunk + j + lane * 8, 8);
                    lanes[lane] = (lanes[lane] ^ value) * 0x100000001B3ull;
                    lanes[lane] ^= lanes[lane] >> 29;
                }
            }
        }
        return lanes[0] ^ (lanes[1] * 3) ^ (lanes[2] * 5) ^ (lanes[3] * 7);
    }

    // Returns whether the contents changed since the last check (always true the first time).
    static bool ContentChanged(ContentCheck& check, const uint8_t* data, size_t size, uint64_t frame, bool first)
    {
        static const bool fullChecks = AR_ENV("AR_FULL_HASH") != nullptr;
        constexpr size_t SMALL = 16384;
        if (first || size <= SMALL || fullChecks)
        {
            uint64_t hash = HashMemory(data, size);
            bool changed = first || hash != check.hash;
            check.hash = hash;
            if (size > SMALL)
                check.sampleHash = SampleHash(data, size);
            check.stableChecks = changed ? 0 : check.stableChecks + 1;
            return changed;
        }

        uint64_t sample = SampleHash(data, size);
        bool full = sample != check.sampleHash || check.stableChecks < 8 || (frame + check.phase) % 30 == 0;
        check.sampleHash = sample;
        if (!full)
            return false;

        uint64_t hash = HashMemory(data, size);
        bool changed = hash != check.hash;
        check.hash = hash;
        check.stableChecks = changed ? 0 : check.stableChecks + 1;
        return changed;
    }

    struct CachedTexture
    {
        Texture texture;
        ContentCheck content;
        uint64_t checkedFrame = UINT64_MAX;
        int keyboardArt = -1; // A controller atlas with a keyboard and mouse version (index into KEYBOARD_ART).
        int gameFont = -1; // One of the title's fonts the overlay draws text with.
    };

    struct PipelineKey
    {
        uint64_t vertexShader;
        uint64_t pixelShader;
        uint32_t blend[4];
        uint32_t colorMask;
        uint32_t depthControl;
        uint32_t stencilMasks;
        uint32_t rasterizer;
        uint8_t topologyType;
        uint8_t geometryShader;
        uint8_t stripCut;
        uint8_t renderTargetCount;
        DXGI_FORMAT renderTargetFormats[4];
        DXGI_FORMAT depthFormat;
    };

    static struct State
    {
        bool initialized = false;
        bool failed = false;

        ComPtr<IDXGIFactory4> factory;
        ComPtr<ID3D12Device> device;

        // Pipeline cache (SetPipelineCacheFile). The serialized blob must outlive the library.
        std::filesystem::path pipelineCachePath;
        std::vector<uint8_t> pipelineCacheBlob;
        ComPtr<ID3D12PipelineLibrary> pipelineLibrary;
        uint32_t pipelinesCreated = 0;
        uint32_t pipelinesLoaded = 0;
        bool pipelineLibraryDirty = false;
        ComPtr<ID3D12CommandQueue> queue;
        // Recorded here, replayed and submitted on the submission thread.
        DeferredList list;
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        uint64_t fenceValue = 0;

        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12Resource> backBuffers[FRAME_COUNT];
        uint32_t backBufferRtvs[FRAME_COUNT];
        uint32_t swapWidth = 0, swapHeight = 0;
        bool tearing = false;
        bool exclusiveFullscreen = false; // Presenting with tearing isn't allowed then.

        FrameResources frames[FRAME_COUNT];
        uint32_t frameIndex = 0;
        uint64_t frameNumber = 0;

        ComPtr<ID3D12Resource> uploadBuffer;
        uint8_t* uploadCpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS uploadGpu = 0;
        uint64_t uploadOffset = 0;

        // Vertex buffer ranges already uploaded since the upload space was last reset: draws sharing a buffer
        // reuse its copy.
        std::unordered_map<uint64_t, uint64_t> vertexUploads;

        struct CachedVertices
        {
            uint64_t offset = 0;
            ContentCheck content;
            uint32_t changes = 0;
            bool valid = false;
        };
        std::unordered_map<uint64_t, CachedVertices> vertexCache;
        uint64_t vertexCacheOffset = VERTEX_CACHE_BASE;
        bool vertexCacheFull = false;

        ComPtr<ID3D12DescriptorHeap> viewHeap, samplerHeap, rtvHeap, dsvHeap;
        uint32_t viewSize = 0, samplerSize = 0, rtvSize = 0, dsvSize = 0;
        DescriptorAllocator views, rtvs, dsvs;
        uint32_t nullRtv = 0;
        uint32_t samplerCount = 0;
        std::unordered_map<uint32_t, uint32_t> samplers;

        ComPtr<ID3D12RootSignature> rootSignature;
        std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
        std::vector<uint8_t> rectangleGeometryShader, pointGeometryShader;
        std::vector<uint8_t> overlayPixelShader;
        ComPtr<ID3D12PipelineState> overlayPipeline;
        std::vector<uint8_t> blitVertexShader, blitPixelShader, presentPixelShader, depthBlitPixelShader, depthCopyPixelShader,
            colorToDepthPixelShader;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> colorToDepthPipelines;
        std::vector<uint8_t> layoutPixelShader, layoutDepthPixelShader;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> layoutPipelines;
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Texture> layoutScratch;
        bool stencilRefSupported = false;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> depthCopyPipelines;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> depthBlitPipelines;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> blitPipelines;
        ComPtr<ID3D12PipelineState> presentPipeline;

        std::unordered_map<RenderTargetKey, Texture, RenderTargetKeyHash> renderTargets;
        // When each render target was last used: overlapping ones copy from those used after them.
        std::unordered_map<RenderTargetKey, uint64_t, RenderTargetKeyHash> renderTargetStamps;
        // Render targets whose EDRAM can overlap each one's (rebuilt when render targets are added).
        std::unordered_map<RenderTargetKey, std::vector<RenderTargetKey>, RenderTargetKeyHash> renderTargetOverlaps;
        bool renderTargetOverlapsDirty = true;

        // Pipeline state the last draw set, to skip setting it again (invalid after anything else records).
        struct DrawState
        {
            bool valid = false;
            uint32_t renderTargetCount = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4]{};
            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
            uint32_t viewportWidth = 0, viewportHeight = 0;
            D3D12_RECT scissor{};
            ID3D12PipelineState* pipeline = nullptr;
            D3D12_PRIMITIVE_TOPOLOGY topology{};
            float blendFactor[4]{};
            uint32_t stencilRef = 0;
        } drawState;

        // Texture lookups made this frame (cleared at resolves, which change resolved textures).
        struct TextureLookup
        {
            uint32_t srv;
            float transform[4];
            uint32_t rawDepth;
        };
        std::unordered_map<uint64_t, TextureLookup> textureLookups;
        // Regions recently written in each render target (in samples), with the stamp they were written under.
        std::unordered_map<RenderTargetKey, std::vector<std::pair<uint64_t, D3D12_RECT>>, RenderTargetKeyHash> renderTargetWrites;
        uint64_t renderTargetClock = 0;
        std::unordered_map<uint32_t, ResolvedTexture> resolvedTextures;
        // AR_GPU_TIMING: timestamps at the start and end of each frame slot, read back once the frame is done.
        ComPtr<ID3D12QueryHeap> timestampHeap;
        ComPtr<ID3D12Resource> timestampReadback;
        uint64_t timestampFrequency = 0;
        bool timestampsPending[FRAME_COUNT] = {};
        // Per frame slot: [0] frame start, [1] frame end, then begin/end pairs of the scopes (GpuScope).
        static constexpr uint32_t TIMESTAMPS_PER_FRAME = 2 + 2 * 4096;
        uint32_t timestampsUsed[FRAME_COUNT] = {};
        // Diagnostics (AR_SHADOW_PIXELS with AR_SHADOW_LOG): samples each logged shadow map draw wrote (occlusion
        // queries), read back once the frame is done and logged as PX lines.
        // The first half for the draws, the second for the draws again without depth testing (AR_SHADOW_PIXELS=3: their
        // rasterized area).
        static constexpr uint32_t OCCLUSIONS_PER_FRAME = 4096;
        static constexpr uint32_t PROBE_OFFSET = OCCLUSIONS_PER_FRAME / 2;
        ComPtr<ID3D12QueryHeap> occlusionHeap;
        ComPtr<ID3D12Resource> occlusionReadback;
        // AR_SHADOW_STATS: pipeline statistics of the same draws (vertices fetched, shader runs, primitives through the
        // clipper), which tell where in the pipeline a draw that wrote nothing got lost.
        // AR_SHADOW_READBACK: the far cascade copied every frame without waiting, then counted a frame later (SM lines).
        ComPtr<ID3D12Resource> shadowReadback[FRAME_COUNT];
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT shadowFootprint[FRAME_COUNT] = {};
        uint64_t shadowCopyFrame[FRAME_COUNT] = {};
        bool shadowCopied[FRAME_COUNT] = {};
        DXGI_FORMAT shadowCopyFormat = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12QueryHeap> statsHeap;
        ComPtr<ID3D12Resource> statsReadback;
        std::vector<std::string> occlusionLabels[FRAME_COUNT];
        // Upload ranges each queried draw reads (offset, size, hash when recorded), checked again once the GPU is done.
        std::vector<std::array<uint64_t, 3>> occlusionInputs[FRAME_COUNT];
        std::vector<std::string> occlusionChanged[FRAME_COUNT]; // Per draw: the inputs found changed ("icsv").
        // AR_SHADOW_GPUCHECK: the GPU copies the inputs of each queried draw to gpuCheckReadback right after the draw,
        // to compare with what the CPU wrote (where each input went, per draw and input).
        static constexpr uint64_t GPU_CHECK_SIZE = 64ull << 20; // Per frame slot.
        ComPtr<ID3D12Resource> gpuCheckReadback;
        std::vector<uint64_t> gpuCheckOffsets[FRAME_COUNT];
        uint64_t gpuCheckUsed[FRAME_COUNT] = {};
        size_t occlusionChecked[FRAME_COUNT] = {}; // Draws whose inputs were checked.
        std::string occlusionLabel; // Set by Draw for the draw being recorded.
        uint32_t shadowResolves = 0; // Shadow map resolves so far in the frame (0 = far cascade draws).
        // Diagnostics (AR_SHADOW_SO=<vertex buffers>): the clip space positions some far cascade draws produce,
        // captured with stream output into slots of soBuffer and read back once the frame is done (SO lines).
        static constexpr uint32_t SO_SLOTS = 16, SO_SLOT_SIZE = 256 * 1024;
        static constexpr uint64_t SO_FILLED_BASE = uint64_t(FRAME_COUNT) * SO_SLOTS * SO_SLOT_SIZE;
        ComPtr<ID3D12Resource> soBuffer, soReadback;
        std::vector<std::string> soLabels[FRAME_COUNT];
        std::string soLabel; // Set by Draw for the draw being recorded.
        // AR_SHADOW_LATE: the probes of the far cascade's draws, drawn just before its resolve instead of right after
        // each draw (whether a draw that rasterized nothing does later in the frame).
        struct LateProbe
        {
            ID3D12PipelineState* pipeline;
            D3D12_GPU_VIRTUAL_ADDRESS systemConstants, vertexConstants, pixelConstants;
            D3D12_INDEX_BUFFER_VIEW indexView;
            bool indexed;
            uint32_t count;
            D3D12_PRIMITIVE_TOPOLOGY topology;
            uint32_t query;
            D3D12_RECT scissor;
            uint32_t viewportWidth, viewportHeight;
        };
        std::vector<LateProbe> lateProbes;
        // AR_SHADOW_DEFAULT_COPY: a video memory buffer as large as the upload buffer; the probe's inputs are copied
        // there by the GPU (same offsets) and the probe reads them from it instead of from upload memory.
        ComPtr<ID3D12Resource> defaultCopy;
        // Experiment (AR_VRAM_UPLOADS): the GPU reads uploads from a video memory mirror of the upload buffer, which a
        // copy command list executed before each submission brings up to date (the ranges written since the last one).
        ComPtr<ID3D12Resource> mirror;
        struct MirrorCopy
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            uint64_t fenceValue = 0;
        };
        std::vector<MirrorCopy> mirrorCopies;
        uint64_t mirrorUploadStart = 0, mirrorCacheStart = 0; // Where this submission's writes started.
        uint64_t mirrorBytes = 0;
        std::vector<uint8_t> scopeCategories[FRAME_COUNT];
        uint32_t scopeDepth = 0;
        // Released textures by description with the frame they were released in. Creating and destroying resources
        // is slow (games resolve different sizes to the same address every frame), so they are reused.
        std::unordered_map<uint64_t, std::vector<std::pair<Texture, uint64_t>>> texturePool;
        std::unordered_map<uint64_t, CompositeTexture> compositeTextures;
        uint64_t resolveVersion = 0;
        std::unordered_map<uint64_t, CachedTexture> textures;
        std::set<uint32_t> warnedFormats;
    } g;

    // Diagnostics (AR_SHADOW_LOG): renderer work other than draws, logged between the shadow map draws (EV lines) of the
    // AR_DUMP_FRAME frames, to see what runs in between.
    static std::unordered_map<uint32_t, std::vector<uint8_t>> g_dsvSnapshots; // Descriptor index -> bytes when created.

    static bool ShadowEventsOn()
    {
        static const bool shadowLog = AR_ENV("AR_SHADOW_LOG") != nullptr;
        if (!shadowLog)
            return false;
        static const int64_t first = AR_ENV("AR_DUMP_FRAME") ? atoll(AR_ENV("AR_DUMP_FRAME")) : 0;
        static const int64_t frames = AR_ENV("AR_DUMP_FRAMES") ? atoll(AR_ENV("AR_DUMP_FRAMES")) : 1;
        return int64_t(g.frameNumber) >= first && int64_t(g.frameNumber) < first + frames;
    }
#define SHADOW_EVENT(...) do { if (ShadowEventsOn()) LOGF_INFO("EV {} {}", g.frameNumber, fmt::format(__VA_ARGS__)); } while (0)

    // Time spent on the command processor thread, logged every 300 frames.
    struct Profile
    {
        double draw, textures, vertices, present, presentWait, gpu, gpuScopes[4];
        double constants, indices, pipeline, record, targets, setup, prepare;
        uint32_t draws, flushes, textureLookups, textureMisses, textureUploads;
        double textureHash, textureDecode;
        uint64_t vertexBytes, uploadBytes;
    } static g_profile;

    // Adds the time since the last lap to a profile slot.
    // Timing of the stages of a draw costs several clock reads per draw, so it is measured only with AR_PROFILE.
    static bool DetailedProfile()
    {
        static const bool enabled = AR_ENV("AR_PROFILE") != nullptr;
        return enabled;
    }

    struct LapTimer
    {
        std::chrono::steady_clock::time_point last = DetailedProfile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        void Lap(double& target)
        {
            if (!DetailedProfile())
                return;
            auto now = std::chrono::steady_clock::now();
            target += std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
        }
    };

    struct ScopedTimer
    {
        double& target;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~ScopedTimer() { target += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
    };
    // A stage of a draw, timed only with AR_PROFILE.
    struct DetailTimer
    {
        double& target;
        std::chrono::steady_clock::time_point start = DetailedProfile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        ~DetailTimer()
        {
            if (DetailedProfile())
                target += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    };

    static D3D12_CPU_DESCRIPTOR_HANDLE CpuView(uint32_t index)
    {
        return { g.viewHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(index) * g.viewSize };
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE CpuRtv(uint32_t index)
    {
        return { g.rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(index) * g.rtvSize };
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE CpuDsv(uint32_t index)
    {
        return { g.dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(index) * g.dsvSize };
    }

    static void Transition(Texture& texture, D3D12_RESOURCE_STATES state)
    {
        if (texture.state == state)
            return;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = texture.state;
        barrier.Transition.StateAfter = state;
        g.list->ResourceBarrier(1, &barrier);
        texture.state = state;
    }

    static void ReleaseTexture(Texture& texture)
    {
        auto& frame = g.frames[g.frameIndex];
        if (texture.poolKey != 0 && texture.resource)
        {
            frame.pooledTextures.push_back(std::move(texture));
            texture = {};
            return;
        }
        if (texture.resource)
            frame.releasedResources.push_back(std::move(texture.resource));
        if (texture.srv != UINT32_MAX)
            frame.releasedSrvs.push_back(texture.srv);
        if (texture.stencilSrv != UINT32_MAX)
            frame.releasedSrvs.push_back(texture.stencilSrv);
        if (texture.rtv != UINT32_MAX)
            frame.releasedRtvs.push_back(texture.rtv);
        if (texture.dsv != UINT32_MAX)
            frame.releasedDsvs.push_back(texture.dsv);
        texture = {};
    }

    // ---- Command submission ----

    static void WaitForFence(uint64_t value)
    {
        if (g.fence->GetCompletedValue() < value)
        {
            g.fence->SetEventOnCompletion(value, g.fenceEvent);
            WaitForSingleObject(g.fenceEvent, INFINITE);
        }
    }

    // Where the frame's upload space starts. Experiment (AR_ROTATE_UPLOADS): 8 MB further on each frame (of 8), so the
    // same addresses don't come back two frames later.
    static uint64_t g_slotCursor[FRAME_COUNT]; // AR_UPLOAD_RING: where each slot's last frame stopped.
    static bool g_slotWrap; // AR_UPLOAD_RING: the slot is full, start it over (the GPU is idle).

    static uint64_t UploadStart()
    {
        uint64_t start = g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT);
        if (g_uploadRing)
        {
            uint64_t cursor = g_slotCursor[g.frameIndex];
            if (!g_slotWrap && cursor > start && cursor < start + UPLOAD_SIZE / FRAME_COUNT - (32ull << 20))
                start = cursor;
            g_slotWrap = false;
            return start;
        }
        static const bool rotate = AR_ENV("AR_ROTATE_UPLOADS") != nullptr;
        if (rotate)
            start += ((g.frameNumber / FRAME_COUNT) % 8) * (8ull << 20);
        return start;
    }

    static void BindHeaps()
    {
        SHADOW_EVENT("bind heaps");
        g.drawState.valid = false;
        g.textureLookups.clear();
        ID3D12DescriptorHeap* heaps[] = { g.viewHeap.Get(), g.samplerHeap.Get() };
        g.list->SetDescriptorHeaps(2, heaps);
        g.list->SetGraphicsRootSignature(g.rootSignature.Get());
        auto views = g.viewHeap->GetGPUDescriptorHandleForHeapStart();
        g.list->SetGraphicsRootDescriptorTable(ROOT_TEXTURES_2D, views);
        g.list->SetGraphicsRootDescriptorTable(ROOT_TEXTURES_3D, views);
        g.list->SetGraphicsRootDescriptorTable(ROOT_TEXTURES_CUBE, views);
        g.list->SetGraphicsRootDescriptorTable(ROOT_SAMPLERS, g.samplerHeap->GetGPUDescriptorHandleForHeapStart());
        g.list->SetGraphicsRootDescriptorTable(ROOT_TEXTURES_UINT, views);
    }

    static void CheckOcclusionInputs(uint32_t slot);
    static uint64_t HashMemory(const uint8_t* data, size_t size);

    static void BeginFrame()
    {
        auto& frame = g.frames[g.frameIndex];
        {
            ScopedTimer timer{ g_profile.presentWait };
            WaitForFence(frame.fenceValue);
        }
        frame.releasedResources.clear();
        if (g.timestampHeap && g.timestampsPending[g.frameIndex])
        {
            uint64_t* times;
            uint64_t base = uint64_t(g.frameIndex) * State::TIMESTAMPS_PER_FRAME;
            D3D12_RANGE range{ base * 8, (base + g.timestampsUsed[g.frameIndex]) * 8 };
            if (SUCCEEDED(g.timestampReadback->Map(0, &range, reinterpret_cast<void**>(&times))))
            {
                times += base;
                uint64_t start = times[0], end = times[1];
                if (end > start)
                    g_profile.gpu += double(end - start) * 1000.0 / double(g.timestampFrequency);
                const auto& categories = g.scopeCategories[g.frameIndex];
                for (size_t i = 0; i < categories.size(); i++)
                {
                    uint64_t scopeStart = times[2 + i * 2], scopeEnd = times[3 + i * 2];
                    if (scopeEnd > scopeStart)
                        g_profile.gpuScopes[categories[i]] += double(scopeEnd - scopeStart) * 1000.0 / double(g.timestampFrequency);
                }
                D3D12_RANGE written{ 0, 0 };
                g.timestampReadback->Unmap(0, &written);
            }
            g.timestampsPending[g.frameIndex] = false;
        }
        if (g.soBuffer && !g.soLabels[g.frameIndex].empty())
        {
            auto& labels = g.soLabels[g.frameIndex];
            uint8_t* data;
            D3D12_RANGE range{ 0, SIZE_T(State::SO_FILLED_BASE + uint64_t(FRAME_COUNT) * State::SO_SLOTS * 16) };
            if (SUCCEEDED(g.soReadback->Map(0, &range, reinterpret_cast<void**>(&data))))
            {
                for (size_t i = 0; i < labels.size(); i++)
                {
                    uint64_t slot = uint64_t(g.frameIndex) * State::SO_SLOTS + i;
                    uint64_t filled;
                    memcpy(&filled, data + State::SO_FILLED_BASE + slot * 16, 8);
                    uint64_t vertices = std::min<uint64_t>(filled / 16, State::SO_SLOT_SIZE / 16);
                    const float* p = reinterpret_cast<const float*>(data + slot * State::SO_SLOT_SIZE);
                    uint32_t inside = 0, nearOut = 0, farOut = 0, sideOut = 0, bad = 0;
                    float lo[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX }, hi[4] = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    for (uint64_t v = 0; v < vertices; v++)
                    {
                        const float* q = p + v * 4;
                        if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2]) || !std::isfinite(q[3]) || q[3] == 0.0f)
                        {
                            bad++;
                            continue;
                        }
                        float n[4] = { q[0] / q[3], q[1] / q[3], q[2] / q[3], q[3] };
                        for (int c = 0; c < 4; c++)
                        {
                            lo[c] = std::min(lo[c], n[c]);
                            hi[c] = std::max(hi[c], n[c]);
                        }
                        bool side = std::abs(n[0]) > 1.0f || std::abs(n[1]) > 1.0f;
                        sideOut += side;
                        nearOut += n[2] < 0.0f;
                        farOut += n[2] > 1.0f;
                        inside += !side && n[2] >= 0.0f && n[2] <= 1.0f;
                    }
                    LOGF_INFO("SO {} vertices {} inside {} z<0 {} z>1 {} xy out {} bad {} x {:.3f}..{:.3f} y {:.3f}..{:.3f} z {:.4f}..{:.4f} w {:.3f}..{:.3f} first {} {} {} {}",
                        labels[i], vertices, inside, nearOut, farOut, sideOut, bad, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3],
                        vertices ? p[0] : 0.0f, vertices ? p[1] : 0.0f, vertices ? p[2] : 0.0f, vertices ? p[3] : 0.0f);
                }
                D3D12_RANGE written{ 0, 0 };
                g.soReadback->Unmap(0, &written);
            }
            labels.clear();
        }
        if (g.shadowCopied[g.frameIndex])
        {
            g.shadowCopied[g.frameIndex] = false;
            const auto& layout = g.shadowFootprint[g.frameIndex].Footprint;
            uint8_t* data;
            if (SUCCEEDED(g.shadowReadback[g.frameIndex]->Map(0, nullptr, reinterpret_cast<void**>(&data))))
            {
                // Texels holding something else than the clear value, in a 16 x 16 grid over the cascade's 1024 x 1024.
                uint32_t size = std::min({ layout.Width, layout.Height, 1024u });
                uint32_t texelBytes = g.shadowCopyFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
                // The clear value: the most common of a sample of texels.
                std::map<uint32_t, uint32_t> sampled;
                for (uint32_t y = 0; y < size; y += 16)
                {
                    for (uint32_t x = 0; x < size; x += 16)
                    {
                        uint32_t word;
                        memcpy(&word, data + layout.RowPitch * y + x * texelBytes, 4);
                        sampled[word]++;
                    }
                }
                uint32_t clear = 0, clearCount = 0;
                for (auto [word, count] : sampled)
                {
                    if (count > clearCount)
                    {
                        clear = word;
                        clearCount = count;
                    }
                }
                std::array<uint32_t, 256> cells{};
                uint64_t total = 0;
                for (uint32_t y = 0; y < size; y++)
                {
                    const uint8_t* row = data + layout.RowPitch * y;
                    for (uint32_t x = 0; x < size; x++)
                    {
                        uint32_t word;
                        memcpy(&word, row + x * texelBytes, 4);
                        if (word != clear)
                        {
                            cells[(y * 16 / size) * 16 + x * 16 / size]++;
                            total++;
                        }
                    }
                }
                std::string text;
                for (uint32_t cell : cells)
                    text += fmt::format(" {}", cell);
                LOGF_INFO("SM {} format {} clear {:08X} written {}{}", g.shadowCopyFrame[g.frameIndex], int(g.shadowCopyFormat), clear, total, text);
                D3D12_RANGE nothing{ 0, 0 };
                g.shadowReadback[g.frameIndex]->Unmap(0, &nothing);
            }
        }
        if (g.occlusionHeap && !g.occlusionLabels[g.frameIndex].empty())
        {
            auto& labels = g.occlusionLabels[g.frameIndex];
            uint64_t* samples;
            uint64_t base = uint64_t(g.frameIndex) * State::OCCLUSIONS_PER_FRAME;
            D3D12_RANGE range{ base * 8, (base + State::OCCLUSIONS_PER_FRAME) * 8 };
            if (SUCCEEDED(g.occlusionReadback->Map(0, &range, reinterpret_cast<void**>(&samples))))
            {
                void* statsMapped = nullptr;
                if (g.statsReadback)
                {
                    D3D12_RANGE statsRange{ 0, size_t(FRAME_COUNT) * State::OCCLUSIONS_PER_FRAME * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) };
                    g.statsReadback->Map(0, &statsRange, &statsMapped);
                }
                samples += base;
                CheckOcclusionInputs(g.frameIndex);
                auto& inputs = g.occlusionInputs[g.frameIndex];
                for (size_t i = 0; i < labels.size(); i++)
                {
                    std::string changed = i < g.occlusionChanged[g.frameIndex].size() ? g.occlusionChanged[g.frameIndex][i] : "";
                    if (g.gpuCheckReadback && i * 4 + 3 < g.gpuCheckOffsets[g.frameIndex].size())
                    {
                        static uint8_t* checkData = [] { uint8_t* data = nullptr; g.gpuCheckReadback->Map(0, nullptr, reinterpret_cast<void**>(&data)); return data; }();
                        std::string differs;
                        for (uint32_t j = 0; j < 4; j++)
                        {
                            uint64_t offset = g.gpuCheckOffsets[g.frameIndex][i * 4 + j];
                            const auto& input = inputs[i * 4 + j];
                            if (offset != UINT64_MAX && checkData && HashMemory(checkData + offset, input[1]) != input[2])
                                differs += "icsv"[j];
                        }
                        if (!differs.empty())
                            changed += " GPU sees different " + differs;
                    }
                    if (g.statsReadback)
                    {
                        const auto* stats = reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(statsMapped) +
                            g.frameIndex * State::OCCLUSIONS_PER_FRAME + i;
                        changed += fmt::format(" stats ia {} {} vs {} clip {} {} psruns {}", stats->IAVertices, stats->IAPrimitives, stats->VSInvocations,
                            stats->CInvocations, stats->CPrimitives, stats->PSInvocations);
                    }
                    static const bool probe = atoi(AR_ENV("AR_SHADOW_PIXELS")) >= 3;
                    if (probe)
                        LOGF_INFO("PX {}{}{} area {} {}", labels[i], changed.empty() ? "" : " changed ", changed,
                            samples[State::PROBE_OFFSET + i], samples[i]);
                    else
                        LOGF_INFO("PX {}{}{} {}", labels[i], changed.empty() ? "" : " changed ", changed, samples[i]);
                }
                if (g.statsReadback)
                {
                    D3D12_RANGE nothing{ 0, 0 };
                    g.statsReadback->Unmap(0, &nothing);
                }
                inputs.clear();
                g.gpuCheckOffsets[g.frameIndex].clear();
                g.gpuCheckUsed[g.frameIndex] = 0;
                g.occlusionChanged[g.frameIndex].clear();
                g.occlusionChecked[g.frameIndex] = 0;
                D3D12_RANGE written{ 0, 0 };
                g.occlusionReadback->Unmap(0, &written);
            }
            labels.clear();
        }
        if (!frame.uploadChecks.empty())
        {
            static uint64_t checked, changed;
            for (const auto& check : frame.uploadChecks)
            {
                checked++;
                if (HashMemory(g.uploadCpu + check.offset, check.size) != check.hash)
                {
                    static uint32_t logged;
                    if (logged++ < 50)
                        LOGF_WARNING("upload memory changed before the GPU was done: {} at offset 0x{:X}, {} bytes, recorded in frame {}",
                            check.what, check.offset, check.size, check.frame);
                    changed++;
                }
            }
            frame.uploadChecks.clear();
            if (g.frameNumber % 300 == 0)
                LOGF_INFO("upload checks: {} checked, {} changed", checked, changed);
        }
        for (Texture& pooled : frame.pooledTextures)
            g.texturePool[pooled.poolKey].emplace_back(std::move(pooled), g.frameNumber);
        frame.pooledTextures.clear();
        if (g.frameNumber % 300 == 0)
        {
            // Unused for 10 seconds: the GPU is long done with them.
            for (auto& [key, list] : g.texturePool)
            {
                std::erase_if(list, [](auto& entry)
                    {
                        if (g.frameNumber - entry.second < 300)
                            return false;
                        Texture& texture = entry.first;
                        g.views.Free(texture.srv);
                        g.views.Free(texture.stencilSrv);
                        g.rtvs.Free(texture.rtv);
                        g.dsvs.Free(texture.dsv);
                        return true;
                    });
            }
        }
        for (uint32_t srv : frame.releasedSrvs)
            g.views.Free(srv);
        frame.releasedSrvs.clear();
        for (uint32_t rtv : frame.releasedRtvs)
            g.rtvs.Free(rtv);
        frame.releasedRtvs.clear();
        for (uint32_t dsv : frame.releasedDsvs)
            g.dsvs.Free(dsv);
        frame.releasedDsvs.clear();

        g.list->ResetAllocator(frame.allocator.Get());
        g.list->Reset(frame.allocator.Get(), nullptr);
        g.uploadOffset = UploadStart();
        g.mirrorUploadStart = g.uploadOffset;
        g.vertexUploads.clear();
        BindHeaps();
        if (g.timestampHeap)
        {
            g.timestampsUsed[g.frameIndex] = 2;
            g.scopeCategories[g.frameIndex].clear();
            g.list->EndQuery(g.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g.frameIndex * State::TIMESTAMPS_PER_FRAME);
        }
    }

    // AR_GPU_TIMING: GPU time of a kind of work (0 layout conversions, 1 render target syncs, 2 resolves, 3 composite
    // textures); nested scopes count for the outermost one.
    struct GpuScope
    {
        uint8_t category;
        uint32_t index = UINT32_MAX;
        explicit GpuScope(uint8_t category) : category(category)
        {
            if (!g.timestampHeap || g.scopeDepth++ != 0 || g.timestampsUsed[g.frameIndex] + 2 > State::TIMESTAMPS_PER_FRAME)
                return;
            index = g.timestampsUsed[g.frameIndex];
            g.timestampsUsed[g.frameIndex] += 2;
            g.scopeCategories[g.frameIndex].push_back(category);
            g.list->EndQuery(g.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g.frameIndex * State::TIMESTAMPS_PER_FRAME + index);
        }
        ~GpuScope()
        {
            if (!g.timestampHeap)
                return;
            g.scopeDepth--;
            if (index != UINT32_MAX)
                g.list->EndQuery(g.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g.frameIndex * State::TIMESTAMPS_PER_FRAME + index + 1);
        }
    };

    // Diagnostics (AR_SHADOW_PIXELS=2): once the GPU is done with them (and before upload space is reused), whether the
    // inputs of the queried draws changed since they were recorded.
    static void CheckOcclusionInputs(uint32_t slot)
    {
        auto& inputs = g.occlusionInputs[slot];
        auto& changed = g.occlusionChanged[slot];
        size_t draws = inputs.size() / 4;
        changed.resize(draws);
        for (size_t i = g.occlusionChecked[slot]; i < draws; i++)
        {
            for (uint32_t j = 0; j < 4; j++)
            {
                const auto& input = inputs[i * 4 + j];
                if (input[1] != 0 && HashMemory(g.uploadCpu + input[0], input[1]) != input[2])
                    changed[i] += "icsv"[j];
            }
        }
        g.occlusionChecked[slot] = draws;
    }

    static void Submit()
    {
        // Experiment (AR_UPLOAD_FENCE): upload memory writes (write combined, possibly non-temporal) made visible
        // before the GPU may start on them.
        static const bool fence = AR_ENV("AR_UPLOAD_FENCE") != nullptr;
        if (fence)
            _mm_mfence();
        if (g.mirror)
        {
            uint64_t completed = g.fence->GetCompletedValue();
            State::MirrorCopy* copy = nullptr;
            for (auto& candidate : g.mirrorCopies)
            {
                if (candidate.fenceValue <= completed)
                {
                    copy = &candidate;
                    break;
                }
            }
            if (!copy)
            {
                copy = &g.mirrorCopies.emplace_back();
                g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy->allocator));
                g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy->allocator.Get(), nullptr, IID_PPV_ARGS(&copy->list));
                copy->list->Close();
            }
            copy->allocator->Reset();
            copy->list->Reset(copy->allocator.Get(), nullptr);
            if (g.uploadOffset > g.mirrorUploadStart)
            {
                copy->list->CopyBufferRegion(g.mirror.Get(), g.mirrorUploadStart, g.uploadBuffer.Get(), g.mirrorUploadStart,
                    g.uploadOffset - g.mirrorUploadStart);
                g.mirrorBytes += g.uploadOffset - g.mirrorUploadStart;
            }
            uint64_t cacheEnd = (g.vertexCacheOffset + 15) & ~15ull;
            if (cacheEnd > g.mirrorCacheStart)
            {
                copy->list->CopyBufferRegion(g.mirror.Get(), g.mirrorCacheStart, g.uploadBuffer.Get(), g.mirrorCacheStart,
                    cacheEnd - g.mirrorCacheStart);
                g.mirrorBytes += cacheEnd - g.mirrorCacheStart;
            }
            copy->list->Close();
            copy->fenceValue = g.fenceValue + 1;
            g.list->ExecuteOther(copy->list.Get());
            g.mirrorUploadStart = g.uploadOffset;
            g.mirrorCacheStart = cacheEnd;
            if (g.frameNumber % 300 == 0 && g.frameIndex == 0)
            {
                LOGF_INFO("video memory mirror: {:.1f} MB copied per frame", double(g.mirrorBytes) / 300.0 / (1 << 20));
                g.mirrorBytes = 0;
            }
        }
        g.list->Close();
        g.list->Execute(++g.fenceValue);
        g.frames[g.frameIndex].fenceValue = g.fenceValue;
    }

    // Submits the work so far and waits for it, so the frame's upload space can be reused.
    static void FlushAndWait()
    {
        g_slotWrap = true;
        if (AR_ENV("AR_SHADOW_LOG"))
            LOGF_INFO("FW {} flush: upload offset {:X} (slice {:X}-{:X}), vertex cache {:X} full {}", g.frameNumber, g.uploadOffset,
                g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT), (g.frameIndex + 1) * (UPLOAD_SIZE / FRAME_COUNT), g.vertexCacheOffset,
                g.vertexCacheFull);
        Submit();
        WaitForFence(g.fenceValue);
        if (g.occlusionHeap)
            CheckOcclusionInputs(g.frameIndex);
        auto& frame = g.frames[g.frameIndex];
        g.list->ResetAllocator(frame.allocator.Get());
        g.list->Reset(frame.allocator.Get(), nullptr);
        g.uploadOffset = UploadStart();
        g.mirrorUploadStart = g.uploadOffset;
        g.vertexUploads.clear();
        BindHeaps();
    }

    static uint64_t HashMemory(const uint8_t* data, size_t size);

    // Diagnostics (AR_CHECK_UPLOADS): remembers what a draw reads from the upload buffer, to check after the GPU has
    // executed the frame that nothing overwrote it in the meantime.
    static bool CheckUploads()
    {
        static const bool enabled = AR_ENV("AR_CHECK_UPLOADS") != nullptr;
        return enabled;
    }
    static void NoteUpload(uint64_t offset, uint64_t size, const char* what);

    static UploadAllocation Upload(uint64_t size, uint64_t alignment)
    {
        uint64_t frameBase = g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT);
        uint64_t frameEnd = frameBase + UPLOAD_SIZE / FRAME_COUNT;
        uint64_t offset = (g.uploadOffset + alignment - 1) & ~(alignment - 1);
        if (offset + size > frameEnd)
        {
            if (size > UPLOAD_SIZE / FRAME_COUNT)
                return { nullptr, 0, 0 };

            FlushAndWait();
            offset = (g.uploadOffset + alignment - 1) & ~(alignment - 1);
        }

        g.uploadOffset = offset + size;
        return { g.uploadCpu + offset, g.uploadGpu + offset, offset };
    }

    static void NoteUpload(uint64_t offset, uint64_t size, const char* what)
    {
        g.frames[g.frameIndex].uploadChecks.push_back({ offset, size, HashMemory(g.uploadCpu + offset, size), g.frameNumber, what });
    }

    template<typename T>
    static D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(const T& value)
    {
        auto allocation = Upload((sizeof(T) + 255) & ~255ull, 256);
        memcpy(allocation.cpu, &value, sizeof(T));
        return allocation.gpu;
    }

    // ---- Initialization ----

    static std::vector<uint8_t> CompileInternal(const char* source, const wchar_t* target, const char* name)
    {
        auto code = CompileHlsl(source, target, name);
        if (code.empty())
            LOGF_ERROR("internal shader {} failed to compile", name);
        return code;
    }

    static constexpr const char* RECTANGLE_GS = R"(
struct Vertex
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    float4 extra : TEXCOORD16;
};

// Rectangle lists give three corners of each rectangle; the fourth is opposite the corner where the two shorter
// edges meet.
[maxvertexcount(4)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> stream)
{
    float2 p0 = input[0].position.xy / input[0].position.w;
    float2 p1 = input[1].position.xy / input[1].position.w;
    float2 p2 = input[2].position.xy / input[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0);
    float d12 = dot(p2 - p1, p2 - p1);
    float d20 = dot(p0 - p2, p0 - p2);
    uint corner = (d01 >= d12 && d01 >= d20) ? 2u : (d12 >= d20 ? 0u : 1u);
    uint a = (corner + 1u) % 3u;
    uint b = (corner + 2u) % 3u;

    Vertex fourth;
    fourth.position = input[a].position + input[b].position - input[corner].position;
    [unroll] for (uint i = 0u; i < 16u; i++)
        fourth.interpolators[i] = input[a].interpolators[i] + input[b].interpolators[i] - input[corner].interpolators[i];
    fourth.extra = input[corner].extra;

    stream.Append(input[corner]);
    stream.Append(input[a]);
    stream.Append(input[b]);
    stream.Append(fourth);
}
)";

    // Point sprites: a square around each point, its diameter from the vertex shader (clamped) or the constant size,
    // with point sprite coordinates from (0, 0) at the top left.
    static constexpr const char* POINT_GS = R"(
cbuffer XeSystemConstants : register(b0)
{
    float4 xe_point : packoffset(c151);
    float4 xe_point_ndc : packoffset(c152);
};

struct Vertex
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    float4 extra : TEXCOORD16;
};

[maxvertexcount(4)]
void main(point Vertex input[1], inout TriangleStream<Vertex> stream)
{
    float2 diameter = input[0].extra.x >= 0.0f ? clamp(input[0].extra.x, xe_point.z, xe_point.w).xx : xe_point.xy;
    float2 radius = diameter * xe_point_ndc.xy * input[0].position.w;
    [unroll] for (uint i = 0u; i < 4u; i++)
    {
        float2 corner = float2(i & 1u, i >> 1u);
        Vertex output = input[0];
        output.position.xy += (corner * 2.0f - 1.0f) * float2(radius.x, -radius.y);
        output.extra.zw = corner;
        stream.Append(output);
    }
}
)";

    static constexpr const char* BLIT_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

    // Copies texels between the same coordinates of two textures, for resolves.
    static constexpr const char* BLIT_PS = R"(
cbuffer BlitConstants : register(b0)
{
    int2 sourceOffset;
    uint sourceIndex;
    uint swapRedBlue;
    float4 scale;
    int2 sampleScale; // Samples per pixel of the source, averaged.
    uint resolutionScale; // sourceOffset and samples are in guest pixels, each resolutionScale texels square.
};

Texture2D<float4> textures[] : register(t0, space0);

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy) / int(resolutionScale), sub = int2(position.xy) % int(resolutionScale);
    int2 coords = (pixel + sourceOffset) * sampleScale;
    float4 color = 0.0f;
    for (int y = 0; y < sampleScale.y; y++)
        for (int x = 0; x < sampleScale.x; x++)
            color += textures[NonUniformResourceIndex(sourceIndex)].Load(int3((coords + int2(x, y)) * int(resolutionScale) + sub, 0));
    color /= float(sampleScale.x * sampleScale.y);
    if (swapRedBlue)
        color = color.bgra;
    return color * scale;
}
)";

    // Raw copy of a depth buffer, as the 32 bit words EDRAM holds: 24 bit depth (unorm, or float with a 4 bit
    // exponent and 20 bit mantissa) above 8 bit stencil, written as 8 bit color components from the low byte up.
    static constexpr const char* DEPTH_BLIT_PS = R"(
cbuffer DepthBlitConstants : register(b0)
{
    int2 sourceOffset;
    uint depthIndex;
    uint stencilIndex;
    uint floatDepth;
    int2 sampleScale;
    uint swapHalves; // Reading depth as colour EDRAM: 40-sample halves of each 80-sample tile are swapped.
    uint resolutionScale; // As in BLIT_PS.
};

Texture2D<float4> textures[] : register(t0, space0);
Texture2D<uint4> uintTextures[] : register(t0, space4);

uint ToFloat24(float value)
{
    value = clamp(value, 0.0f, 1.99999988f);
    uint bits = asuint(value);
    int exponent = int(bits >> 23) - 127 + 15;
    if (exponent <= 0)
        return uint(value * exp2(34.0f)) & 0xFFFFFu; // Denormal: mantissa scaled by 2^(14 + 20).
    return (uint(exponent) << 20) | ((bits >> 3) & 0xFFFFFu);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy) / int(resolutionScale), sub = int2(position.xy) % int(resolutionScale);
    int2 guest = (pixel + sourceOffset) * sampleScale;
    if (swapHalves)
        guest.x += (uint(guest.x) % 80u) < 40u ? 40 : -40;
    int3 coords = int3(guest * int(resolutionScale) + sub, 0);
    float depth = textures[NonUniformResourceIndex(depthIndex)].Load(coords).r;
    uint stencil = uintTextures[NonUniformResourceIndex(stencilIndex)].Load(coords).g;
    uint depth24 = floatDepth ? ToFloat24(depth) : uint(round(saturate(depth) * 16777215.0f));
    uint raw = (depth24 << 8) | (stencil & 0xFFu);
    return float4(raw & 0xFFu, (raw >> 8) & 0xFFu, (raw >> 16) & 0xFFu, raw >> 24) / 255.0f;
}
)";

    // Copies depth and stencil between depth buffers (stencil only where the GPU lets shaders output it).
    static constexpr const char* DEPTH_COPY_PS = R"(
cbuffer DepthCopyConstants : register(b0)
{
    int2 sourceOffset;
    uint depthIndex;
    uint stencilIndex;
};

Texture2D<float4> textures[] : register(t0, space0);
Texture2D<uint4> uintTextures[] : register(t0, space4);

float main(float4 position : SV_Position
#ifdef STENCIL_REF
    , out uint stencil : SV_StencilRef
#endif
    ) : SV_Depth
{
    int3 coords = int3(int2(position.xy) + sourceOffset, 0);
#ifdef STENCIL_REF
    stencil = uintTextures[NonUniformResourceIndex(stencilIndex)].Load(coords).g;
#endif
    return textures[NonUniformResourceIndex(depthIndex)].Load(coords).r;
}
)";

    // Depth buffers from 8888 color written over their EDRAM: the words hold 24 bit depth (unorm or float with a 4 bit
    // exponent) above 8 bit stencil, from the low byte up.
    static constexpr const char* COLOR_TO_DEPTH_PS = R"(
cbuffer ColorToDepthConstants : register(b0)
{
    int2 sourceOffset;
    uint colorIndex;
    uint floatDepth;
    uint resolutionScale; // sourceOffset is in host texels; the swap works on guest samples.
};

Texture2D<float4> textures[] : register(t0, space0);

float main(float4 position : SV_Position
#ifdef STENCIL_REF
    , out uint stencil : SV_StencilRef
#endif
    ) : SV_Depth
{
    // Depth tiles have their 40-sample halves swapped relative to colour tiles; games writing depth through colour
    // render targets swap them in their shaders.
    int2 coords = int2(position.xy) + sourceOffset;
    int scale = int(resolutionScale);
    coords.x += (uint(coords.x / scale) % 80u) < 40u ? 40 * scale : -40 * scale;
    uint4 bytes = uint4(round(saturate(textures[NonUniformResourceIndex(colorIndex)].Load(int3(coords, 0))) * 255.0f));
#ifdef STENCIL_REF
    stencil = bytes.x;
#endif
    uint depth24 = bytes.y | (bytes.z << 8u) | (bytes.w << 16u);
    if (floatDepth == 0u)
        return float(depth24) / 16777215.0f;
    uint exponent = depth24 >> 20u;
    float mantissa = float(depth24 & 0xFFFFFu) / 1048576.0f;
    return exponent == 0u ? mantissa * exp2(-14.0f) : (1.0f + mantissa) * exp2(float(exponent) - 15.0f);
}
)";

    // Reorders a render target's texels between sample layouts (see Texture::layout). EDRAM interleaves the samples
    // of 2x2 pixel blocks (Xenia's render target cache, canonical sample coordinates):
    //   2x: u = (x & ~2) | (sample << 1), v = ((y >> 1) << 2) | (x & 2) | (y & 1)
    //   4x: u = ((x >> 1) << 2) | (sampleX << 1) | (x & 1), likewise v with y and sampleY.
    static constexpr const char* LAYOUT_PS = R"(
cbuffer LayoutConstants : register(b0)
{
    uint sourceIndex;
    uint stencilIndex;
    uint fromLayout;
    uint toLayout;
    uint resolutionScale; // Samples are resolutionScale texels square.
};


Texture2D<float4> textures[] : register(t0, space0);
Texture2D<uint4> uintTextures[] : register(t0, space4);

uint2 ToCanonical(uint layout, uint2 h)
{
    if (layout == 1u)
    {
        uint x = h.x, y = h.y >> 1u, sample = h.y & 1u;
        return uint2((x & ~2u) | (sample << 1u), ((y >> 1u) << 2u) | (x & 2u) | (y & 1u));
    }
    if (layout == 2u)
    {
        uint2 p = h >> 1u, sample = h & 1u;
        return ((p >> 1u) << 2u) | (sample << 1u) | (p & 1u);
    }
    return h;
}

uint2 FromCanonical(uint layout, uint2 c)
{
    if (layout == 1u)
    {
        uint x = (c.x & ~3u) | (c.y & 2u) | (c.x & 1u), sample = (c.x >> 1u) & 1u;
        uint y = ((c.y >> 2u) << 1u) | (c.y & 1u);
        return uint2(x, y * 2u + sample);
    }
    if (layout == 2u)
    {
        uint2 sample = (c >> 1u) & 1u, p = ((c >> 2u) << 1u) | (c & 1u);
        return p * 2u + sample;
    }
    return c;
}

uint2 Reorder(uint2 h)
{
    uint2 sample = h / resolutionScale, sub = h % resolutionScale;
    return FromCanonical(fromLayout, ToCanonical(toLayout, sample)) * resolutionScale + sub;
}

#ifdef DEPTH
float main(float4 position : SV_Position
#ifdef STENCIL_REF
    , out uint stencil : SV_StencilRef
#endif
    ) : SV_Depth
{
    int3 coords = int3(Reorder(uint2(position.xy)), 0);
#ifdef STENCIL_REF
    stencil = uintTextures[NonUniformResourceIndex(stencilIndex)].Load(coords).g;
#endif
    return textures[NonUniformResourceIndex(sourceIndex)].Load(coords).r;
}
#else
float4 main(float4 position : SV_Position) : SV_Target
{
    int3 coords = int3(Reorder(uint2(position.xy)), 0);
    return textures[NonUniformResourceIndex(sourceIndex)].Load(coords);
}
#endif
)";

    static constexpr const char* PRESENT_PS = R"(
cbuffer PresentConstants : register(b0)
{
    float2 viewportOrigin;
    float2 viewportSize;
    uint sourceIndex;
    uint samplerIndex;
    uint swapRedBlue; // The front buffer's fetch constant reads it as BGRA.
};

Texture2D<float4> textures[] : register(t0, space0);
SamplerState samplers[] : register(s0, space0);

float4 main(float4 position : SV_Position) : SV_Target
{
    float2 uv = (position.xy - viewportOrigin) / viewportSize;
    float3 color = textures[sourceIndex].SampleLevel(samplers[samplerIndex], uv, 0.0f).rgb;
    return float4(swapRedBlue ? color.bgr : color, 1.0f);
}
)";

    // Art on top of the picture: a rectangle of a texture's alpha, tinted.
    static constexpr const char* OVERLAY_PS = R"(
cbuffer OverlayConstants : register(b0)
{
    float2 viewportOrigin;
    float2 viewportSize;
    float2 uvOrigin;
    float2 uvSize;
    float4 tint;
    uint sourceIndex;
    uint samplerIndex;
    uint picture; // 1: the texture's colours, rather than its alpha in the tint's colour. 2: that alpha with hard edges.
};

Texture2D<float4> textures[] : register(t0, space0);
SamplerState samplers[] : register(s0, space0);

float4 main(float4 position : SV_Position) : SV_Target
{
    float2 uv = uvOrigin + (position.xy - viewportOrigin) / viewportSize * uvSize;
    float4 texel = textures[sourceIndex].SampleLevel(samplers[samplerIndex], uv, 0.0f);
    if (picture == 2u)
        return float4(tint.rgb, tint.a * saturate((texel.a - 0.3725f) / 0.157f));
    return picture != 0u ? float4(texel.rgb, tint.a) : float4(tint.rgb, tint.a * texel.a);
}
)";

    static bool CreateRootSignature()
    {
        D3D12_DESCRIPTOR_RANGE ranges[5]{};
        for (uint32_t i = 0; i < 3; i++)
        {
            ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = UINT_MAX;
            ranges[i].RegisterSpace = i;
        }
        ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[3].NumDescriptors = UINT_MAX;
        ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[4].NumDescriptors = UINT_MAX;
        ranges[4].RegisterSpace = 4;

        D3D12_ROOT_PARAMETER parameters[ROOT_PARAMETER_COUNT]{};
        parameters[ROOT_SYSTEM_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[ROOT_SYSTEM_CONSTANTS].Descriptor.ShaderRegister = 0;
        parameters[ROOT_VERTEX_FLOAT_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[ROOT_VERTEX_FLOAT_CONSTANTS].Descriptor.ShaderRegister = 1;
        parameters[ROOT_VERTEX_FLOAT_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[ROOT_PIXEL_FLOAT_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[ROOT_PIXEL_FLOAT_CONSTANTS].Descriptor.ShaderRegister = 1;
        parameters[ROOT_PIXEL_FLOAT_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[ROOT_VERTEX_DATA].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[ROOT_VERTEX_DATA].Descriptor.RegisterSpace = 3;
        parameters[ROOT_VERTEX_DATA].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        for (uint32_t i = 0; i < 5; i++)
        {
            auto& parameter = parameters[ROOT_TEXTURES_2D + i];
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &ranges[i];
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = ROOT_PARAMETER_COUNT;
        desc.pParameters = parameters;
        if (AR_ENV("AR_SHADOW_SO"))
            desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        {
            LOGF_ERROR("failed to serialize the root signature: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "");
            return false;
        }

        return SUCCEEDED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSignature)));
    }

    static void CreateSwapChainBuffers()
    {
        for (uint32_t i = 0; i < FRAME_COUNT; i++)
        {
            g.swapChain->GetBuffer(i, IID_PPV_ARGS(&g.backBuffers[i]));
            g.device->CreateRenderTargetView(g.backBuffers[i].Get(), nullptr, CpuRtv(g.backBufferRtvs[i]));
        }
    }

    static bool Initialize()
    {
        if (g.initialized || g.failed)
            return g.initialized;

        g.failed = true;
        HWND hwnd = window::Create("Anarchy Reigns");
        g_scale = std::clamp<uint32_t>(g_videoSettings.renderScale, 1, 4);

        // AR_D3D12_DEBUG: the D3D12 debug layer, with its messages in the log (each message once);
        // AR_D3D12_GBV also validates resource states as the GPU executes (much slower).
        if (AR_ENV("AR_D3D12_DEBUG"))
        {
            ComPtr<ID3D12Debug1> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                if (AR_ENV("AR_D3D12_GBV"))
                    debug->SetEnableGPUBasedValidation(TRUE);
            }
        }

        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory))))
            return false;

        ComPtr<IDXGIFactory6> factory6;
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(g.factory.As(&factory6)))
            factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
        {
            LOGF_ERROR("failed to create a Direct3D 12 device");
            return false;
        }

        if (AR_ENV("AR_D3D12_DEBUG"))
        {
            ComPtr<ID3D12InfoQueue1> infoQueue;
            DWORD cookie;
            if (SUCCEEDED(g.device.As(&infoQueue)))
            {
                infoQueue->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                    LPCSTR description, void*)
                    {
                        if (severity > D3D12_MESSAGE_SEVERITY_WARNING)
                            return;
                        static std::mutex mutex;
                        static std::unordered_map<uint32_t, uint32_t> counts;
                        std::lock_guard lock(mutex);
                        uint32_t count = ++counts[uint32_t(id)];
                        if (count == 1 || count == 100 || count == 10000)
                            LOGF_WARNING("D3D12 {} {} (seen {} times): {}", severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning",
                                uint32_t(id), count, description);
                    }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            }
            else
            {
                LOGF_WARNING("the D3D12 debug layer's message callback isn't available; messages go to the debugger");
            }
        }

        if (adapter)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            std::wstring name = desc.Description;
            LOGF_INFO("Direct3D 12 adapter: {}", std::string(name.begin(), name.end()));
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));

        // Pipeline library from the last run, or an empty one (also when the driver changed).
        if (ComPtr<ID3D12Device1> device1; !g.pipelineCachePath.empty() && SUCCEEDED(g.device.As(&device1)))
        {
            std::ifstream file(g.pipelineCachePath, std::ios::binary | std::ios::ate);
            if (file)
            {
                g.pipelineCacheBlob.resize(size_t(file.tellg()));
                file.seekg(0);
                file.read(reinterpret_cast<char*>(g.pipelineCacheBlob.data()), g.pipelineCacheBlob.size());
            }
            if (g.pipelineCacheBlob.empty() || FAILED(device1->CreatePipelineLibrary(g.pipelineCacheBlob.data(),
                g.pipelineCacheBlob.size(), IID_PPV_ARGS(&g.pipelineLibrary))))
            {
                g.pipelineCacheBlob.clear();
                if (FAILED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g.pipelineLibrary))))
                    LOGF_WARNING("pipeline libraries are not supported; pipelines are created every run");
            }
            else
            {
                LOGF_INFO("pipeline cache loaded ({} KB)", g.pipelineCacheBlob.size() / 1024);
            }
        }
        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
            LOGF_WARNING("the GPU only supports resource binding tier 1; rendering may fail");
        g.stencilRefSupported = options.PSSpecifiedStencilRefSupported;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));

        for (auto& frame : g.frames)
            g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&g.list.real));
        g.list.real->Close();
        g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
        g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        auto createHeap = [](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, ComPtr<ID3D12DescriptorHeap>& heap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc{ type, count, shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
                g.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
                return g.device->GetDescriptorHandleIncrementSize(type);
            };
        g.viewSize = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, VIEW_HEAP_SIZE, true, g.viewHeap);
        g.samplerSize = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE, true, g.samplerHeap);
        g.rtvSize = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP_SIZE, false, g.rtvHeap);
        g.dsvSize = createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP_SIZE, false, g.dsvHeap);
        g.views.capacity = VIEW_HEAP_SIZE;
        g.views.next = 3;
        g.rtvs.capacity = RTV_HEAP_SIZE;
        g.dsvs.capacity = DSV_HEAP_SIZE;

        // Null views for texture slots without a texture.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
        nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(nullptr, &nullDesc, CpuView(NULL_SRV_2D));
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        nullDesc.Texture3D.MipLevels = 1;
        g.device->CreateShaderResourceView(nullptr, &nullDesc, CpuView(NULL_SRV_3D));
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        nullDesc.TextureCube.MipLevels = 1;
        g.device->CreateShaderResourceView(nullptr, &nullDesc, CpuView(NULL_SRV_CUBE));

        D3D12_RENDER_TARGET_VIEW_DESC nullRtvDesc{};
        nullRtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g.nullRtv = g.rtvs.Allocate();
        g.device->CreateRenderTargetView(nullptr, &nullRtvDesc, CpuRtv(g.nullRtv));

        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        // Experiment (AR_UPLOAD_WRITE_BACK): cache coherent system memory (CPU write-back pages the GPU snoops) instead
        // of the write-combined upload heap.
        if (AR_ENV("AR_UPLOAD_WRITE_BACK"))
            uploadHeap = { D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0 };
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = VERTEX_CACHE_BASE + VERTEX_CACHE_SIZE; // 4 KB padding: constants are read as 4 KB.
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.uploadBuffer))))
        {
            LOGF_ERROR("failed to create the upload buffer");
            return false;
        }
        g.uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&g.uploadCpu));
        g.uploadGpu = g.uploadBuffer->GetGPUVirtualAddress();
        LOGF_INFO("upload buffer: GPU address {:X}-{:X}", g.uploadGpu, g.uploadGpu + VERTEX_CACHE_BASE + VERTEX_CACHE_SIZE);
        if (AR_ENV("AR_VRAM_UPLOADS"))
        {
            D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
            if (SUCCEEDED(g.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(&g.mirror))))
            {
                g.uploadGpu = g.mirror->GetGPUVirtualAddress();
                g.mirrorCacheStart = VERTEX_CACHE_BASE;
                LOGF_INFO("uploads are read from a video memory mirror");
            }
        }
        if (AR_ENV("AR_SHADOW_DEFAULT_COPY"))
        {
            D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
            if (FAILED(g.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&g.defaultCopy))))
                LOGF_ERROR("failed to create the default copy buffer");
        }

        if (!CreateRootSignature())
            return false;

        g.rectangleGeometryShader = CompileInternal(RECTANGLE_GS, L"gs_6_0", "rectangle_gs");
        g.pointGeometryShader = CompileInternal(POINT_GS, L"gs_6_0", "point_gs");
        g.blitVertexShader = CompileInternal(BLIT_VS, L"vs_6_0", "blit_vs");
        g.blitPixelShader = CompileInternal(BLIT_PS, L"ps_6_0", "blit_ps");
        g.presentPixelShader = CompileInternal(PRESENT_PS, L"ps_6_0", "present_ps");
        g.overlayPixelShader = CompileInternal(OVERLAY_PS, L"ps_6_0", "overlay_ps");
        g.depthBlitPixelShader = CompileInternal(DEPTH_BLIT_PS, L"ps_6_0", "depth_blit_ps");
        g.layoutPixelShader = CompileInternal(LAYOUT_PS, L"ps_6_0", "layout_ps");
        g.layoutDepthPixelShader = CompileInternal((std::string("#define DEPTH\n") +
            (g.stencilRefSupported ? "#define STENCIL_REF\n" : "") + LAYOUT_PS).c_str(), L"ps_6_0", "layout_depth_ps");
        g.colorToDepthPixelShader = CompileInternal(
            (std::string(g.stencilRefSupported ? "#define STENCIL_REF\n" : "") + COLOR_TO_DEPTH_PS).c_str(), L"ps_6_0", "color_to_depth_ps");
        g.depthCopyPixelShader = CompileInternal(
            (std::string(g.stencilRefSupported ? "#define STENCIL_REF\n" : "") + DEPTH_COPY_PS).c_str(), L"ps_6_0", "depth_copy_ps");
        if (g.blitVertexShader.empty() || g.blitPixelShader.empty() || g.presentPixelShader.empty())
            return false;

        BOOL tearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(g.factory.As(&factory5)))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        g.tearing = tearing;

        window::GetBufferSize(g.swapWidth, g.swapHeight);
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.Width = g.swapWidth;
        swapDesc.Height = g.swapHeight;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = FRAME_COUNT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.Flags = g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        ComPtr<IDXGISwapChain1> swapChain;
        if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue.Get(), hwnd, &swapDesc, nullptr, nullptr, &swapChain)))
        {
            LOGF_ERROR("failed to create the swap chain");
            return false;
        }
        g.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        swapChain.As(&g.swapChain);
        for (auto& rtv : g.backBufferRtvs)
            rtv = g.rtvs.Allocate();
        CreateSwapChainBuffers();

        if (g_videoSettings.mode == DisplayMode::Fullscreen)
        {
            // Exclusive fullscreen at the chosen resolution (the nearest mode the monitor has).
            if (SUCCEEDED(g.swapChain->SetFullscreenState(TRUE, nullptr)))
            {
                DXGI_MODE_DESC mode{};
                mode.Width = g_videoSettings.width;
                mode.Height = g_videoSettings.height;
                mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                g.swapChain->ResizeTarget(&mode);
                g.exclusiveFullscreen = true;
                g.swapWidth = 0; // The buffers are resized to the new mode on the first present.
            }
            else
            {
                LOGF_WARNING("exclusive fullscreen is not available; using a borderless window");
            }
        }

        g.frameIndex = g.swapChain->GetCurrentBackBufferIndex();
        g.list.queue = g.queue.Get();
        g.list.fence = g.fence.Get();
        g.list.swapChain = g.swapChain.Get();
        // AR_SINGLE_THREAD records straight into the command list on the GPU thread.
        if (AR_ENV("AR_GPU_TIMING"))
        {
            D3D12_QUERY_HEAP_DESC queryDesc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FRAME_COUNT * State::TIMESTAMPS_PER_FRAME, 0 };
            D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC readbackDesc{};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Width = uint64_t(FRAME_COUNT) * State::TIMESTAMPS_PER_FRAME * 8;
            readbackDesc.Height = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels = 1;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&g.timestampHeap))) ||
                FAILED(g.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&g.timestampReadback))) || FAILED(g.queue->GetTimestampFrequency(&g.timestampFrequency)))
            {
                g.timestampHeap.Reset();
            }
        }
        if (AR_ENV("AR_SHADOW_SO"))
        {
            D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT }, readbackHeap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bufferDesc{};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = State::SO_FILLED_BASE + uint64_t(FRAME_COUNT) * State::SO_SLOTS * 16;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&g.soBuffer))) ||
                FAILED(g.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&g.soReadback))))
            {
                g.soBuffer.Reset();
            }
        }
        if (AR_ENV("AR_SHADOW_PIXELS"))
        {
            D3D12_QUERY_HEAP_DESC queryDesc{ D3D12_QUERY_HEAP_TYPE_OCCLUSION, FRAME_COUNT * State::OCCLUSIONS_PER_FRAME, 0 };
            D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC readbackDesc{};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Width = uint64_t(FRAME_COUNT) * State::OCCLUSIONS_PER_FRAME * 8;
            readbackDesc.Height = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels = 1;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (AR_ENV("AR_SHADOW_GPUCHECK"))
            {
                D3D12_RESOURCE_DESC checkDesc = readbackDesc;
                checkDesc.Width = State::GPU_CHECK_SIZE * FRAME_COUNT;
                g.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &checkDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&g.gpuCheckReadback));
            }
            if (FAILED(g.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&g.occlusionHeap))) ||
                FAILED(g.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&g.occlusionReadback))))
            {
                g.occlusionHeap.Reset();
            }
            if (g.occlusionHeap && AR_ENV("AR_SHADOW_STATS"))
            {
                D3D12_QUERY_HEAP_DESC statsDesc{ D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, FRAME_COUNT * State::OCCLUSIONS_PER_FRAME, 0 };
                D3D12_RESOURCE_DESC statsBuffer = readbackDesc;
                statsBuffer.Width = uint64_t(FRAME_COUNT) * State::OCCLUSIONS_PER_FRAME * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
                if (FAILED(g.device->CreateQueryHeap(&statsDesc, IID_PPV_ARGS(&g.statsHeap))) ||
                    FAILED(g.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &statsBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(&g.statsReadback))))
                {
                    g.statsHeap.Reset();
                }
            }
        }
        g.list.Start(!AR_ENV("AR_SINGLE_THREAD"));
        if (!AR_ENV("AR_SINGLE_THREAD"))
            LOGF_INFO("command submission runs on its own thread");
        g.initialized = true;
        g.failed = false;
        BeginFrame();
        return true;
    }

    bool TraceThisFrame();
    static void SaveScreenshot(struct Texture& texture, uint32_t width, uint32_t height, const std::filesystem::path& path,
        bool swapRedBlue = false);

    // ---- Textures ----

    static Texture CreateTexture(uint32_t width, uint32_t height, uint32_t depth, bool cube, DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        uint64_t poolWords[4] = { (uint64_t(width) << 32) | height, (uint64_t(depth) << 1) | (cube ? 1 : 0), uint64_t(format), uint64_t(flags) };
        uint64_t poolKey = HashMemory(reinterpret_cast<const uint8_t*>(poolWords), sizeof(poolWords)) | 1;
        static const bool noPool = AR_ENV("AR_NO_TEXTURE_POOL") != nullptr;
        if (!noPool)
        {
            auto pooled = g.texturePool.find(poolKey);
            if (pooled != g.texturePool.end() && !pooled->second.empty())
            {
                Texture texture = std::move(pooled->second.back().first);
                pooled->second.pop_back();
                texture.layout = 0;
                Transition(texture, state);
                return texture;
            }
        }

        // Diagnostics (AR_LOG_CREATES=<from frame>): textures created during play.
        static const int64_t logCreatesFrom = AR_ENV("AR_LOG_CREATES") ? atoll(AR_ENV("AR_LOG_CREATES")) : -1;
        if (logCreatesFrom >= 0 && int64_t(g.frameNumber) >= logCreatesFrom)
        {
            static uint32_t logged;
            if (logged++ < 300)
                LOGF_INFO("create texture frame {}: {}x{}x{} format {} flags {}", g.frameNumber, width, height, depth, uint32_t(format), uint32_t(flags));
        }

        Texture texture;
        texture.format = format;
        texture.width = width;
        texture.height = height;
        texture.poolKey = noPool ? 0 : poolKey;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = (!cube && depth > 1) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = UINT16(depth);
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;

        // Depth buffers are sampled by depth resolves, so they are created typeless.
        DXGI_FORMAT srvFormat = format;
        if (format == DXGI_FORMAT_D24_UNORM_S8_UINT)
        {
            desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
            srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        }
        else if (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        {
            desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
            srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        }
        else
        {
            desc.Format = format;
        }

        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture.resource))))
        {
            LOGF_ERROR("failed to create a {}x{}x{} texture of format {}", width, height, depth, uint32_t(format));
            return {};
        }

        texture.state = state;
        texture.srv = g.views.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = srvFormat;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (cube)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = 1;
        }
        else if (depth > 1)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MipLevels = 1;
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
        }
        g.device->CreateShaderResourceView(texture.resource.Get(), &srv, CpuView(texture.srv));

        // Stencil is the second plane of depth formats.
        if (format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC stencil = srv;
            stencil.Format = format == DXGI_FORMAT_D24_UNORM_S8_UINT ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            stencil.Texture2D.PlaneSlice = 1;
            texture.stencilSrv = g.views.Allocate();
            g.device->CreateShaderResourceView(texture.resource.Get(), &stencil, CpuView(texture.stencilSrv));
        }

        if (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        {
            texture.rtv = g.rtvs.Allocate();
            g.device->CreateRenderTargetView(texture.resource.Get(), nullptr, CpuRtv(texture.rtv));
        }
        if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        {
            texture.dsv = g.dsvs.Allocate();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = format;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            g.device->CreateDepthStencilView(texture.resource.Get(), &dsv, CpuDsv(texture.dsv));
            // Diagnostics (AR_SHADOW_LOG): the descriptor as written, to notice it being overwritten later.
            if (AR_ENV("AR_SHADOW_LOG"))
            {
                auto& snapshot = g_dsvSnapshots[texture.dsv];
                snapshot.assign(reinterpret_cast<const uint8_t*>(CpuDsv(texture.dsv).ptr),
                    reinterpret_cast<const uint8_t*>(CpuDsv(texture.dsv).ptr) + g.dsvSize);
            }
        }

        return texture;
    }

    static uint64_t HashMemory(const uint8_t* data, size_t size)
    {
        // Four independent lanes, so the multiplications overlap.
        uint64_t lanes[4] = { 0xCBF29CE484222325ull, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull };
        size_t i = 0;
        for (; i + 32 <= size; i += 32)
        {
            for (int lane = 0; lane < 4; lane++)
            {
                uint64_t value;
                memcpy(&value, data + i + lane * 8, 8);
                lanes[lane] = (lanes[lane] ^ value) * 0x100000001B3ull;
                lanes[lane] ^= lanes[lane] >> 29;
            }
        }
        uint64_t hash = lanes[0] ^ (lanes[1] * 3) ^ (lanes[2] * 5) ^ (lanes[3] * 7) ^ size;
        for (; i < size; i++)
            hash = (hash ^ data[i]) * 0x100000001B3ull;
        return hash;
    }

    static void UploadTexture(Texture& texture, const DecodedTexture& decoded)
    {
        uint64_t sliceSize = uint64_t(decoded.rowPitch) * decoded.rowCount;
        auto allocation = Upload(sliceSize * decoded.depth, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        SHADOW_EVENT("texture upload {} bytes", sliceSize * decoded.depth);
        if (allocation.cpu == nullptr)
            return;

        memcpy(allocation.cpu, decoded.data.data(), decoded.data.size());
        Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);

        bool volume = texture.resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        uint32_t copies = volume ? 1 : decoded.depth;
        for (uint32_t slice = 0; slice < copies; slice++)
        {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = g.uploadBuffer.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset = allocation.offset + slice * sliceSize;
            source.PlacedFootprint.Footprint = { decoded.format, decoded.width, decoded.height, volume ? decoded.depth : 1, decoded.rowPitch };

            D3D12_TEXTURE_COPY_LOCATION dest{};
            dest.pResource = texture.resource.Get();
            dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dest.SubresourceIndex = slice;
            g.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
        }

        Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    static uint32_t BytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM: return 1;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT: return 2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_FLOAT: return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        default: return 4;
        }
    }

    // Finds the resolved texture holding an address at the start of a row: the one starting there, else one it is
    // inside. rowBytes 0 accepts any row pitch.
    static ResolvedTexture* FindResolved(uint32_t address, uint32_t rowBytes, uint32_t& row)
    {
        row = 0;
        auto exact = g.resolvedTextures.find(address);
        if (exact != g.resolvedTextures.end() && (rowBytes == 0 || exact->second.rowBytes == rowBytes))
            return &exact->second;

        for (auto& [base, resolved] : g.resolvedTextures)
        {
            uint64_t end = uint64_t(base) + uint64_t(resolved.rowBytes) * (resolved.texture.height / g_scale);
            if (address > base && address < end && (rowBytes == 0 || resolved.rowBytes == rowBytes) &&
                (address - base) % resolved.rowBytes == 0)
            {
                row = (address - base) / resolved.rowBytes;
                return &resolved;
            }
        }

        return nullptr;
    }

    // Returns the descriptor index for the texture a fetch constant describes. transform receives the UV scale and
    // offset that map the fetch's coordinates onto the host texture.
    //
    // Depth formats (24_8 and 24_8_FLOAT) fetched from depth resolves hold raw words; rawDepth receives 1 or 2 for
    // the shader to decode them as unorm or float depth.
    static uint32_t GetTextureUncached(const TextureFetch& fetch, uint32_t dimension, float* transform, uint32_t& rawDepth);

    static uint32_t GetTexture(const TextureFetch& fetch, uint32_t dimension, float* transform, uint32_t& rawDepth)
    {
        uint32_t keyWords[7];
        memcpy(keyWords, fetch.words, sizeof(fetch.words));
        keyWords[6] = dimension;
        uint64_t key = HashMemory(reinterpret_cast<const uint8_t*>(keyWords), sizeof(keyWords));
        g_profile.textureLookups++;
        auto it = g.textureLookups.find(key);
        if (it == g.textureLookups.end())
        {
            g_profile.textureMisses++;
            State::TextureLookup lookup;
            lookup.srv = GetTextureUncached(fetch, dimension, lookup.transform, lookup.rawDepth);
            it = g.textureLookups.emplace(key, lookup).first;
        }
        else if (it->second.srv != NULL_SRV_2D && it->second.srv != NULL_SRV_3D && it->second.srv != NULL_SRV_CUBE)
        {
            // Still transitioned for this draw: resolves render into resolved textures between draws.
            for (auto& [base, resolved] : g.resolvedTextures)
            {
                if (resolved.texture.srv == it->second.srv)
                {
                    Transition(resolved.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    break;
                }
            }
        }
        memcpy(transform, it->second.transform, sizeof(it->second.transform));
        rawDepth = it->second.rawDepth;
        return it->second.srv;
    }

    // Fetches running past the end of a resolved texture into the resolved textures after it: copies the rows the
    // fetch covers into one texture. Returns UINT32_MAX when the fetch doesn't continue into another resolve.
    static uint32_t GetCompositeTexture(const TextureFetch& fetch, ResolvedTexture& first, uint32_t row, float* transform, uint32_t& rawDepth)
    {
        struct Part
        {
            uint32_t address;
            ResolvedTexture* resolved;
            uint32_t row, rows, destRow;
        };
        std::vector<Part> parts;
        uint32_t address = fetch.BaseAddress(), remaining = fetch.Height(), destRow = 0;
        ResolvedTexture* current = &first;
        while (current && remaining != 0)
        {
            uint32_t rows = std::min(remaining, current->texture.height / g_scale - row);
            parts.push_back({ address, current, row, rows, destRow });
            remaining -= rows;
            destRow += rows;
            address += rows * current->rowBytes;
            if (remaining == 0)
                break;
            uint32_t nextRow;
            ResolvedTexture* next = FindResolved(address, current->rowBytes, nextRow);
            if (!next || next->texture.format != first.texture.format || next->texture.width != first.texture.width ||
                next->rawDepth != first.rawDepth)
                break;
            current = next;
            row = nextRow;
        }
        if (parts.size() < 2)
            return UINT32_MAX;

        uint32_t keyWords[3] = { fetch.BaseAddress(), fetch.Height(), first.rowBytes };
        auto& composite = g.compositeTextures[HashMemory(reinterpret_cast<const uint8_t*>(keyWords), sizeof(keyWords))];
        uint32_t height = fetch.Height() * g_scale;
        if (composite.texture.resource && (composite.texture.format != first.texture.format ||
            composite.texture.width != first.texture.width || composite.texture.height != height))
        {
            ReleaseTexture(composite.texture);
            composite.parts.clear();
        }
        if (!composite.texture.resource)
        {
            composite.texture = CreateTexture(first.texture.width, height, 1, false, first.texture.format, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST);
            if (!composite.texture.resource)
                return UINT32_MAX;
            composite.parts.clear();
        }

        // Copied again when a part has been resolved since (or the parts are different textures now).
        std::vector<std::pair<uint32_t, uint64_t>> versions;
        for (const Part& part : parts)
            versions.emplace_back(part.address, part.resolved->version);
        if (versions != composite.parts)
        {
            SHADOW_EVENT("composite {:08X}", fetch.BaseAddress());
            GpuScope scope{ 3 };
            Transition(composite.texture, D3D12_RESOURCE_STATE_COPY_DEST);
            for (const Part& part : parts)
            {
                Transition(part.resolved->texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION source{ part.resolved->texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION dest{ composite.texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_BOX box{ 0, part.row * g_scale, 0, part.resolved->texture.width, (part.row + part.rows) * g_scale, 1 };
                g.list->CopyTextureRegion(&dest, 0, part.destRow * g_scale, 0, &source, &box);
            }
            composite.parts = std::move(versions);
            if (TraceThisFrame())
                LOGF_INFO("composite texture 0x{:08X} {}x{} from {} resolves", fetch.BaseAddress(), fetch.Width(), fetch.Height(), parts.size());
        }

        auto& texture = composite.texture;
        transform[0] = float(fetch.Width() * g_scale) / float(texture.width);
        transform[1] = 1.0f;
        transform[3] = 0.0f;
        rawDepth |= 4;
        if (first.rawDepth && (fetch.Format() == 22 || fetch.Format() == 23))
            rawDepth = fetch.Format() == 23 ? 2 : 1;
        Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return texture.srv;
    }

    // The title's button icons and controller diagram have keyboard and mouse versions, shown while those are the input in
    // use (see tools/mkb_art/make_mkb_art.py). They're recognised by their contents, wherever the title loads them.
#include "mkb_art.inl"
    struct KeyboardArt
    {
        uint64_t guestHash;
        uint32_t size;
        const uint8_t* alpha;
        size_t alphaSize;
        uint32_t height = 0; // 0: as wide as tall.
        Texture texture;
    };
    static KeyboardArt KEYBOARD_ART[] = {
        { 0x6588FD3326A35AA6ull, 256, g_mkbIcons, sizeof(g_mkbIcons) },
        { 0x86739066C3B5D4FFull, 512, g_mkbDiagram, sizeof(g_mkbDiagram) },
        { 0ull, 256, g_mkbMenuIcons, sizeof(g_mkbMenuIcons) }, // Icons again, in menus: picked by GetTextureUncached.
        { 0ull, 1024, g_overlayAtlas, sizeof(g_overlayAtlas), 256 },
        { 0ull, 1280, g_controlsKeyboard, sizeof(g_controlsKeyboard), 720 },
        { 0ull, 1280, g_controlsController, sizeof(g_controlsController), 720 },
        { 0ull, 256, g_mkbStunIcons, sizeof(g_mkbStunIcons) }, // Icons again, while the stun prompts show.
        { 0ull, 1280, g_controlsKeysLit, sizeof(g_controlsKeysLit), 720 }, // The keyboard page's keys lit.
        { 0ull, 1024, g_pageFont, sizeof(g_pageFont), 128 }, // The keyboard page's text.
        { 0ull, 256, g_mkbViewerIcons, sizeof(g_mkbViewerIcons) }, // Icons again, in the Gallery's character viewer.
    };

    // The controls page for the controller is the title's own diagram: a picture of a loading screen, taken the first time one
    // shows (with the controller's art, whatever is in use) and kept in the user's folder. Nothing of it is in this project.
    static const char* CONTROLLER_DIAGRAM_FILE = "user/controller_diagram.bmp";

    static constexpr uint64_t GAME_ICONS_HASH = 0x1AEBE5D5E582F6BAull, GAME_STENCIL_HASH = 0xE242DB80AEB462ADull;

    // The title's fonts for the Options screen, for the rows added to it (see kbm::GetOverlay): recognised by their contents.
    static constexpr uint64_t GAME_FONT_HASHES[4] = { 0x219D81F7D336DB88ull, 0x146C441743931623ull, GAME_ICONS_HASH, GAME_STENCIL_HASH };
    static CachedTexture* g_gameFonts[4];

    static bool GameFontReady(int index)
    {
        return g_gameFonts[index] && g_gameFonts[index]->gameFont == index && g_gameFonts[index]->texture.resource;
    }

    bool TitleFontsAvailable()
    {
        return GameFontReady(1) && GameFontReady(3);
    }

    bool GameIconsAvailable()
    {
        return g_gameFonts[2] && g_gameFonts[2]->gameFont == 2 && g_gameFonts[2]->texture.resource;
    }

    bool GameFontsAvailable()
    {
        for (int i = 0; i < 2; i++)
        {
            if (!g_gameFonts[i] || g_gameFonts[i]->gameFont != i || !g_gameFonts[i]->texture.resource)
                return false;
        }
        return true;
    }

    static void NoteGameFont(CachedTexture& entry, const DecodedTexture& decoded, uint32_t address)
    {
        entry.gameFont = -1;
        bool icons = decoded.width == 128 && decoded.height == 64;
        bool stencil = decoded.format == DXGI_FORMAT_BC3_UNORM && decoded.width == 512 && decoded.height == 512;
        if (!icons && !stencil && (decoded.format != DXGI_FORMAT_BC3_UNORM || !((decoded.width == 256 && decoded.height == 256) || (decoded.width == 2048 && decoded.height == 1024))))
            return;
        uint64_t hash = HashMemory(decoded.data.data(), decoded.data.size());
        static const bool log = AR_ENV("AR_LOG_ATLAS_HASHES") != nullptr;
        if (log && (icons || stencil))
            LOGF_INFO("atlas {:08X} {}x{} format {} hash {:016X}", address, decoded.width, decoded.height, int(decoded.format), hash);
        for (int i = 0; i < 4; i++)
        {
            if (hash == GAME_FONT_HASHES[i])
            {
                entry.gameFont = i;
                g_gameFonts[i] = &entry;
            }
        }
    }
    static uint64_t g_diagramSeenFrame = UINT64_MAX;
    static bool g_diagramCapturing;
    static Texture g_diagramTexture;

    static int FindKeyboardArt(const DecodedTexture& decoded, uint32_t address)
    {
        static const bool log = AR_ENV("AR_LOG_ATLAS_HASHES") != nullptr;
        if (decoded.format == DXGI_FORMAT_BC3_UNORM && log && decoded.width == 2048 && decoded.height == 1024)
            LOGF_INFO("atlas {:08X} {}x{} hash {:016X}", address, decoded.width, decoded.height, HashMemory(decoded.data.data(), decoded.data.size()));
        if (decoded.format != DXGI_FORMAT_BC3_UNORM || decoded.width != decoded.height || (decoded.width != 256 && decoded.width != 512))
            return -1;
        uint64_t hash = HashMemory(decoded.data.data(), decoded.data.size());
        if (log)
            LOGF_INFO("atlas {:08X} {}x{} hash {:016X}", address, decoded.width, decoded.height, hash);
        for (size_t i = 0; i < std::size(KEYBOARD_ART); i++)
        {
            if (KEYBOARD_ART[i].guestHash == hash)
                return int(i);
        }
        return -1;
    }

    static const Texture& GetKeyboardArt(int index)
    {
        KeyboardArt& art = KEYBOARD_ART[index];
        if (!art.texture.resource)
        {
            // (Pages are wider than tall: art.size is the width, the height comes from the data.)
            DecodedTexture decoded{};
            decoded.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uint32_t height = art.height ? art.height : art.size;
            decoded.width = art.size;
            decoded.height = height;
            decoded.depth = 1;
            decoded.rowPitch = art.size * 4;
            decoded.rowCount = height;
            decoded.data.resize(size_t(art.size) * height * 4);
            size_t pixel = 0, total = size_t(art.size) * height;
            for (size_t i = 0; i + 1 < art.alphaSize && pixel < total; i += 2)
            {
                for (uint32_t run = uint32_t(art.alpha[i]) + 1; run != 0 && pixel < total; run--, pixel++)
                {
                    uint8_t* out = &decoded.data[pixel * 4];
                    // Like the originals: white where there's anything, black elsewhere (the UI's shadow pass shows otherwise).
                    out[0] = out[1] = out[2] = art.alpha[i + 1] != 0 ? 255 : 0;
                    out[3] = art.alpha[i + 1];
                }
            }
            art.texture = CreateTexture(art.size, height, 1, false, decoded.format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (art.texture.resource)
                UploadTexture(art.texture, decoded);
        }
        return art.texture;
    }

    static uint32_t GetTextureUncached(const TextureFetch& fetch, uint32_t dimension, float* transform, uint32_t& rawDepth)
    {
        rawDepth = 0;
        transform[0] = transform[1] = 1.0f;
        transform[2] = transform[3] = 0.0f;
        uint32_t nullSrv = dimension == 3 ? NULL_SRV_CUBE : dimension == 2 ? NULL_SRV_3D : NULL_SRV_2D;
        if ((fetch.words[0] & 3) != 2) // Not a texture fetch constant.
            return nullSrv;
        static const bool nullCubes = AR_ENV("AR_NULL_CUBE") != nullptr;
        if (nullCubes && dimension == 3)
            return nullSrv;

        // Resolved render targets are used directly.
        uint32_t row;
        ResolvedTexture* resolved = dimension < 2 ? FindResolved(fetch.BaseAddress(), 0, row) : nullptr;
        static const bool noComposite = AR_ENV("AR_NO_COMPOSITE") != nullptr;
        if (resolved && !noComposite && row + fetch.Height() > resolved->texture.height / g_scale)
        {
            if (uint32_t srv = GetCompositeTexture(fetch, *resolved, row, transform, rawDepth); srv != UINT32_MAX)
                return srv;
        }
        if (resolved)
        {
            auto& texture = resolved->texture;
            transform[0] = float(fetch.Width() * g_scale) / float(texture.width);
            transform[1] = float(fetch.Height() * g_scale) / float(texture.height);
            transform[3] = float(row * g_scale) / float(texture.height);
            rawDepth |= 4; // Scaled: unnormalized coordinates and offsets are in guest texels.
            if (resolved->rawDepth && (fetch.Format() == 22 || fetch.Format() == 23))
                rawDepth = fetch.Format() == 23 ? 2 : 1;
            Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return texture.srv;
        }

        // Diagnostics (AR_LOG_UNMATCHED): textures overlapping resolved memory but not matched to a resolved texture
        // are decoded from guest memory, which resolves don't write.
        static const bool logUnmatched = AR_ENV("AR_LOG_UNMATCHED") != nullptr;
        if (logUnmatched)
        {
            uint32_t start = fetch.BaseAddress(), end = start + std::max<uint32_t>(GetGuestTextureSize(fetch), 1);
            for (auto& [base, other] : g.resolvedTextures)
            {
                uint64_t otherEnd = uint64_t(base) + uint64_t(other.rowBytes) * other.texture.height;
                if (start < otherEnd && end > base)
                {
                    static std::set<uint64_t> logged;
                    if (logged.insert((uint64_t(start) << 32) | (fetch.Format() << 24) | (fetch.Width() & 0xFFF) << 12 | (fetch.Height() & 0xFFF)).second && logged.size() < 200)
                        LOGF_WARNING("texture 0x{:08X} {}x{} fmt {} dim {} tiled {} (pitch {}) overlaps resolved 0x{:08X} {}x{} fmt {} but isn't matched",
                            start, fetch.Width(), fetch.Height(), fetch.Format(), dimension, fetch.Tiled(), fetch.Pitch(), base,
                            other.texture.width, other.texture.height, other.format);
                    break;
                }
            }
        }

        if (GetHostTextureFormat(fetch.Format()) == DXGI_FORMAT_UNKNOWN)
        {
            if (g.warnedFormats.insert(fetch.Format()).second)
                LOGF_WARNING("texture format {} is not supported", fetch.Format());
            return nullSrv;
        }

        // Fetch constants also hold sampler state, which doesn't change the texture.
        uint32_t keyWords[6] = { fetch.words[0] & 0xFFC003FF, fetch.words[1], fetch.words[2], dimension, 0, fetch.words[5] & 0xFFFFF600 };
        uint64_t key = HashMemory(reinterpret_cast<const uint8_t*>(keyWords), sizeof(keyWords));
        auto& entry = g.textures[key];

        if (entry.checkedFrame != g.frameNumber)
        {
            entry.checkedFrame = g.frameNumber;
            uint32_t size = GetGuestTextureSize(fetch);
            if (entry.content.phase == 0)
                entry.content.phase = uint32_t(key % 30) + 1;
            bool newTexture = !entry.texture.resource;
            LapTimer hashLap;
            bool changed = ContentChanged(entry.content, g_memory.Translate<uint8_t>(PhysicalToVirtual(fetch.BaseAddress())), size,
                g.frameNumber, !entry.texture.resource);
            hashLap.Lap(g_profile.textureHash);
            if (changed)
            {
                DetailTimer decodeTimer{ g_profile.textureDecode };
                g_profile.textureUploads++;
                DecodedTexture decoded;
                if (!DecodeTexture(fetch, decoded))
                    return nullSrv;

                if (!entry.texture.resource)
                {
                    entry.texture = CreateTexture(decoded.width, decoded.height, decoded.depth, dimension == 3, decoded.format,
                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                    if (!entry.texture.resource)
                        return nullSrv;
                }

                bool firstUpload = newTexture;
                static int changeLogs;
                if (!firstUpload && AR_ENV("AR_TRACE_FRAME") && changeLogs++ < 40)
                    LOGF_INFO("texture {:08X} changed in frame {}", fetch.BaseAddress(), g.frameNumber);
                UploadTexture(entry.texture, decoded);
                entry.keyboardArt = FindKeyboardArt(decoded, fetch.BaseAddress());
                NoteGameFont(entry, decoded, fetch.BaseAddress());

                // AR_DUMP_TEXTURES: every texture when first uploaded, from that frame on (0: from the start) for AR_DUMP_TEXTURE_FRAMES
                // frames; compressed ones as they are, in .dds files.
                static const int64_t dumpTexturesFrom = AR_ENV("AR_DUMP_TEXTURES") ? atoll(AR_ENV("AR_DUMP_TEXTURES")) : -1;
                static const int64_t dumpTextureFrames = AR_ENV("AR_DUMP_TEXTURE_FRAMES") ? atoll(AR_ENV("AR_DUMP_TEXTURE_FRAMES")) : INT64_MAX / 2;
                bool dumpTexture = firstUpload && dumpTexturesFrom >= 0 && int64_t(g.frameNumber) >= dumpTexturesFrom &&
                    int64_t(g.frameNumber) < dumpTexturesFrom + dumpTextureFrames;
                if (dumpTexture && decoded.format != DXGI_FORMAT_R8G8B8A8_UNORM && decoded.depth <= 1)
                {
                    struct DdsHeader
                    {
                        uint32_t magic = 0x20534444, size = 124, flags = 0x1007, height, width, pitch = 0, depth = 0, mips = 1, reserved[11] = {};
                        uint32_t formatSize = 32, formatFlags = 4, fourCc = 0x30315844, bits = 0, masks[4] = {};
                        uint32_t caps = 0x1000, caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
                        uint32_t dxgi, dimension = 3, misc = 0, arraySize = 1, misc2 = 0;
                    } header;
                    header.height = decoded.height;
                    header.width = decoded.width;
                    header.dxgi = uint32_t(decoded.format);
                    std::error_code ec;
                    std::filesystem::create_directories("screenshots", ec);
                    std::ofstream file(fmt::format("screenshots/texture_{:08X}.dds", fetch.BaseAddress()), std::ios::binary);
                    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                    file.write(reinterpret_cast<const char*>(decoded.data.data()), decoded.data.size());
                    LOGF_INFO("texture {:08X} {}x{} format {} dumped as dds", fetch.BaseAddress(), decoded.width, decoded.height, int(decoded.format));
                }
                if (TraceThisFrame() || dumpTexture)
                {
                    uint32_t minimum = 255, maximum = 0;
                    for (uint8_t value : decoded.data)
                    {
                        minimum = std::min<uint32_t>(minimum, value);
                        maximum = std::max<uint32_t>(maximum, value);
                    }
                    LOGF_INFO("texture {:08X} {}x{} format {} tiled {} endian {} pitch {}: bytes {}-{}", fetch.BaseAddress(), decoded.width,
                        decoded.height, fetch.Format(), fetch.Tiled(), fetch.Endian(), fetch.Pitch(), minimum, maximum);
                    if (fetch.Height() == 1)
                    {
                        const uint8_t* raw = g_memory.Translate<uint8_t>(PhysicalToVirtual(fetch.BaseAddress()));
                        std::string hex;
                        for (uint32_t i = 0; i < 1024; i++)
                            hex += fmt::format("{:02X}{}", raw[i], (i & 3) == 3 ? " " : "");
                        LOGF_INFO("  raw: {}", hex);
                    }
                    if (decoded.format == DXGI_FORMAT_R8G8B8A8_UNORM)
                        SaveScreenshot(entry.texture, decoded.width, decoded.height, fmt::format("screenshots/texture_{:08X}.bmp", fetch.BaseAddress()));
                }
            }
        }

        if (entry.keyboardArt == 1)
            g_diagramSeenFrame = g.frameNumber;
        if (entry.keyboardArt >= 0 && entry.texture.resource && kbm::Active() && !g_diagramCapturing)
        {
            int artIndex = entry.keyboardArt;
            if (artIndex == 0)
                artIndex = kbm::StunPromptVisible() ? 6 : kbm::ViewerActive() ? 9 : kbm::InMenu() ? 2 : 0;
            const Texture& art = GetKeyboardArt(artIndex);
            if (art.resource)
                return art.srv;
        }
        return entry.texture.resource ? entry.texture.srv : nullSrv;
    }

    // ---- Samplers ----

    static uint32_t CreateSampler(uint32_t key, const D3D12_SAMPLER_DESC& desc)
    {
        auto it = g.samplers.find(key);
        if (it != g.samplers.end())
            return it->second;

        if (g.samplerCount >= SAMPLER_HEAP_SIZE)
            return 0;

        uint32_t index = g.samplerCount++;
        g.device->CreateSampler(&desc, { g.samplerHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(index) * g.samplerSize });
        g.samplers.emplace(key, index);
        return index;
    }

    static uint32_t GetSampler(const TextureFetch& fetch)
    {
        uint32_t clampX = (fetch.words[0] >> 10) & 7, clampY = (fetch.words[0] >> 13) & 7, clampZ = (fetch.words[0] >> 16) & 7;
        uint32_t mag = (fetch.words[3] >> 19) & 3, min = (fetch.words[3] >> 21) & 3, mip = (fetch.words[3] >> 23) & 3;
        uint32_t aniso = (fetch.words[3] >> 25) & 7;
        uint32_t border = fetch.words[5] & 3;
        uint32_t key = clampX | (clampY << 3) | (clampZ << 6) | (mag << 9) | (min << 11) | (mip << 13) | (aniso << 15) | (border << 18);

        static constexpr D3D12_TEXTURE_ADDRESS_MODE addressModes[] = { D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE };

        D3D12_SAMPLER_DESC desc{};
        auto type = [](uint32_t filter) { return filter == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT; };
        if (aniso >= 2 && aniso <= 5)
        {
            desc.Filter = D3D12_FILTER_ANISOTROPIC;
            desc.MaxAnisotropy = 1u << (aniso - 1);
        }
        else
        {
            desc.Filter = D3D12_ENCODE_BASIC_FILTER(type(min), type(mag), type(mip), D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        }
        desc.AddressU = addressModes[clampX];
        desc.AddressV = addressModes[clampY];
        desc.AddressW = addressModes[clampZ];
        desc.MaxLOD = mip == 2 ? 0.0f : D3D12_FLOAT32_MAX;
        desc.MipLODBias = float(int32_t(fetch.words[4] << 10) >> 22) / 32.0f;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        float borderValue = border == 1 ? 1.0f : 0.0f;
        desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = borderValue;
        desc.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
        return CreateSampler(key, desc);
    }

    static uint32_t GetLinearClampSampler()
    {
        D3D12_SAMPLER_DESC desc{};
        desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MaxLOD = D3D12_FLOAT32_MAX;
        return CreateSampler(UINT32_MAX, desc);
    }

    // ---- Render targets ----

    static DXGI_FORMAT GetColorTargetFormat(uint32_t format)
    {
        switch (format)
        {
        case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;          // 8_8_8_8, 8_8_8_8_GAMMA
        case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;      // 2_10_10_10, 2_10_10_10_AS_10_10_10_10
        case 3: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;     // 2_10_10_10_FLOAT
        case 4: case 6: return DXGI_FORMAT_R16G16_FLOAT;            // 16_16, 16_16_FLOAT
        case 5: case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;      // 16_16_16_16, 16_16_16_16_FLOAT
        case 14: return DXGI_FORMAT_R32_FLOAT;
        case 15: return DXGI_FORMAT_R32G32_FLOAT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    static ID3D12PipelineState* GetDepthCopyPipeline(DXGI_FORMAT format, bool fromColor = false);
    static ID3D12PipelineState* GetBlitPipeline(DXGI_FORMAT format, bool present, bool depth);
    static void SetViewportAndScissor(uint32_t width, uint32_t height, D3D12_RECT scissor);

    // EDRAM tiles are 80x16 samples of 32 bits (40x16 of 64 bits).
    static uint32_t EdramTileWidth(uint32_t format, bool depth)
    {
        return !depth && (format == 5 || format == 7 || format == 15) ? 40 : 80;
    }

    static D3D12_SHADER_BYTECODE Bytecode(const std::vector<uint8_t>& code);

    static ID3D12PipelineState* GetLayoutPipeline(DXGI_FORMAT format, bool depth)
    {
        auto& pipeline = g.layoutPipelines[uint32_t(format) | (depth ? 0x10000u : 0u)];
        if (pipeline)
            return pipeline.Get();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g.rootSignature.Get();
        desc.VS = Bytecode(g.blitVertexShader);
        desc.PS = Bytecode(depth ? g.layoutDepthPixelShader : g.layoutPixelShader);
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.SampleDesc.Count = 1;
        if (depth)
        {
            desc.DepthStencilState.DepthEnable = TRUE;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            if (g.stencilRefSupported)
            {
                desc.DepthStencilState.StencilEnable = TRUE;
                desc.DepthStencilState.StencilReadMask = 0xFF;
                desc.DepthStencilState.StencilWriteMask = 0xFF;
                desc.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                    D3D12_COMPARISON_FUNC_ALWAYS };
                desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
            }
            desc.DSVFormat = format;
        }
        else
        {
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = format;
        }
        if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
            LOGF_ERROR("failed to create the layout pipeline for format {}", uint32_t(format));
        return pipeline.Get();
    }

    // Reorders a render target's texels into another sample layout, through a scratch copy.
    static void ConvertLayout(Texture& texture, uint32_t layout)
    {
        static const bool disabled = AR_ENV("AR_NO_LAYOUT") != nullptr;
        if (disabled || texture.layout == layout || !texture.resource)
            return;
        SHADOW_EVENT("layout {} -> {} ({}x{})", texture.layout, layout, texture.width, texture.height);
        GpuScope scope{ 0 };

        bool depth = texture.dsv != UINT32_MAX;
        auto& scratch = g.layoutScratch[{ uint32_t(texture.format), texture.width, texture.height }];
        if (!scratch.resource)
        {
            scratch = CreateTexture(texture.width, texture.height, 1, false, texture.format,
                depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST);
            if (!scratch.resource)
                return;
        }

        Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(scratch, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyResource(scratch.resource.Get(), texture.resource.Get());
        Transition(scratch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (depth)
        {
            Transition(texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            auto dsv = CpuDsv(texture.dsv);
            g.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        }
        else
        {
            Transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rtv = CpuRtv(texture.rtv);
            g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
        SetViewportAndScissor(texture.width, texture.height, { 0, 0, LONG(texture.width), LONG(texture.height) });
        g.list->SetPipelineState(GetLayoutPipeline(texture.format, depth));
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        struct
        {
            uint32_t sourceIndex;
            uint32_t stencilIndex;
            uint32_t fromLayout;
            uint32_t toLayout;
            uint32_t resolutionScale;
        } constants{ scratch.srv, scratch.stencilSrv, texture.layout, layout, g_scale };
        g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
        g.list->DrawInstanced(3, 1, 0, 0);
        texture.layout = layout;
    }

    static D3D12_RECT UnionRect(const D3D12_RECT& a, const D3D12_RECT& b)
    {
        return { std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom) };
    }

    // Diagnostics (AR_SHADOW_LOG): EDRAM syncs into the shadow map depth buffer (base 128, pitch 1040), in the logged frames.
    static bool ShadowSyncLog(const RenderTargetKey& key)
    {
        static const bool shadowLog = AR_ENV("AR_SHADOW_LOG") != nullptr;
        if (!shadowLog || !key.depth || key.base != 128 || key.pitch != 1040)
            return false;
        static const int64_t first = AR_ENV("AR_DUMP_FRAME") ? atoll(AR_ENV("AR_DUMP_FRAME")) : 0;
        static const int64_t frames = AR_ENV("AR_DUMP_FRAMES") ? atoll(AR_ENV("AR_DUMP_FRAMES")) : 1;
        return int64_t(g.frameNumber) >= first && int64_t(g.frameNumber) < first + frames;
    }

    // Notes a region written in a render target, under its current stamp.
    static void RecordWrite(const RenderTargetKey& key, D3D12_RECT rect)
    {
        if (rect.left >= rect.right || rect.top >= rect.bottom)
            return;

        auto& writes = g.renderTargetWrites[key];
        uint64_t stamp = g.renderTargetStamps[key];
        if (!writes.empty() && writes.back().first == stamp)
            writes.back().second = UnionRect(writes.back().second, rect);
        else
            writes.push_back({ stamp, rect });

        if (writes.size() > 32)
            writes.erase(writes.begin());
    }

    // Render targets whose EDRAM overlaps (same pitch and format, bases whole tile rows apart; games point
    // surfaces into each other, like a tile's depth buffer inside the full screen one a clear wrote) are separate
    // host textures, so a render target being used gets the overlapping rows of the ones used after it.
    static void SyncRenderTarget(const RenderTargetKey& key, Texture& texture)
    {
        uint64_t& stamp = g.renderTargetStamps[key];
        uint32_t pitchTiles = (key.pitch + EdramTileWidth(key.format, key.depth) - 1) / EdramTileWidth(key.format, key.depth);

        if (g.renderTargetOverlapsDirty)
        {
            g.renderTargetOverlapsDirty = false;
            g.renderTargetOverlaps.clear();
            for (auto& [a, ignored] : g.renderTargets)
            {
                auto& list = g.renderTargetOverlaps[a];
                uint32_t tiles = (a.pitch + EdramTileWidth(a.format, a.depth) - 1) / EdramTileWidth(a.format, a.depth);
                for (auto& [b, ignored2] : g.renderTargets)
                {
                    bool depthIntoColor = !a.depth && b.depth && (a.format == 0 || a.format == 1);
                    static const bool noColorIntoDepth = AR_ENV("AR_NO_COLOR_TO_DEPTH") != nullptr;
                    bool colorIntoDepth = !noColorIntoDepth && a.depth && a.format == 1 && !b.depth && (b.format == 0 || b.format == 1);
                    if (b == a || b.pitch != a.pitch || (!depthIntoColor && !colorIntoDepth && (b.depth != a.depth || b.format != a.format)))
                        continue;
                    int32_t delta = int32_t(b.base) - int32_t(a.base);
                    if (delta % int32_t(tiles) == 0 && std::abs(delta / int32_t(tiles)) * 16 < int32_t(RENDER_TARGET_HEIGHT))
                        list.push_back(b);
                }
            }
        }

        static thread_local std::vector<std::pair<uint64_t, std::pair<const RenderTargetKey*, Texture*>>> newer;
        newer.clear();
        for (const RenderTargetKey& candidate : g.renderTargetOverlaps[key])
        {
            auto found = g.renderTargets.find(candidate);
            if (found == g.renderTargets.end())
                continue;
            const RenderTargetKey& otherKey = found->first;
            Texture& other = found->second;
            // 8888 color targets also take the raw words of depth buffers over them (games clear color by clearing
            // depth at the same EDRAM address).
            bool depthIntoColor = !key.depth && otherKey.depth && (key.format == 0 || key.format == 1);
            static const bool noColorIntoDepth = AR_ENV("AR_NO_COLOR_TO_DEPTH") != nullptr;
            // Only float depth buffers: the game restores its effects pass depth (D24FS8) by drawing the saved words as
            // colour, while its unorm depth buffers share EDRAM with colour for combined clears, where converting
            // colour into depth corrupts what is later drawn over it.
            bool colorIntoDepth = !noColorIntoDepth && key.depth && key.format == 1 && !otherKey.depth &&
                (otherKey.format == 0 || otherKey.format == 1);
            if (otherKey == key || otherKey.pitch != key.pitch ||
                (!depthIntoColor && !colorIntoDepth && (otherKey.depth != key.depth || otherKey.format != key.format)))
                continue;
            int32_t delta = int32_t(otherKey.base) - int32_t(key.base);
            uint64_t otherStamp = g.renderTargetStamps[otherKey];
            if (otherStamp > stamp && delta % int32_t(pitchTiles) == 0 &&
                std::abs(delta / int32_t(pitchTiles)) * 16 < int32_t(RENDER_TARGET_HEIGHT))
            {
                newer.push_back({ otherStamp, { &otherKey, &other } });
            }
        }
        std::sort(newer.begin(), newer.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        GpuScope scope{ 1 };

        for (auto& [otherStamp, entry] : newer)
        {
            auto& [otherKey, other] = entry;
            SHADOW_EVENT("sync base {} pitch {} <- base {}", key.base, key.pitch, otherKey->base);
            if (ShadowSyncLog(key) && key.depth == otherKey->depth)
                LOGF_INFO("SY {} base {:X} pitch {} format {} <- base {:X} format {} (whole rows)", g.frameNumber, key.base, key.pitch,
                    key.format, otherKey->base, otherKey->format);
            ConvertLayout(texture, 0);
            ConvertLayout(*other, 0);
            // The other target's row 0 is at this one's row `rows` (in samples).
            int32_t rows = (int32_t(otherKey->base) - int32_t(key.base)) / int32_t(pitchTiles) * 16;
            uint32_t sourceTop = uint32_t(std::max(0, -rows)), destTop = uint32_t(std::max(0, rows));
            uint32_t height = RENDER_TARGET_HEIGHT - uint32_t(std::abs(rows));

            if (key.depth != otherKey->depth)
            {
                // Only what the other target wrote since this one was last used: elsewhere its texture doesn't hold
                // what EDRAM does (this target's own contents).
                D3D12_RECT written{};
                bool any = false;
                for (auto& [writeStamp, rect] : g.renderTargetWrites[*otherKey])
                {
                    if (writeStamp > stamp)
                    {
                        written = any ? UnionRect(written, rect) : rect;
                        any = true;
                    }
                }
                if (!any)
                    continue;

                written.top = std::max<LONG>(written.top + rows, LONG(destTop));
                written.bottom = std::min<LONG>(written.bottom + rows, LONG(destTop + height));
                if (written.top >= written.bottom || written.left >= written.right)
                    continue;

                if (TraceThisFrame() || ShadowSyncLog(key))
                    LOGF_INFO("  {} base {:X} format {} <- {} base {:X} format {}: rows {} rect {},{}-{},{} frame {}", key.depth ? "depth" : "color",
                        key.base, key.format, otherKey->depth ? "depth" : "color", otherKey->base, otherKey->format, rows,
                        written.left, written.top, written.right, written.bottom, g.frameNumber);
                if (key.depth)
                {
                    Transition(*other, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    Transition(texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    auto dsv = CpuDsv(texture.dsv);
                    g.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                    SetViewportAndScissor(texture.width, texture.height, Scaled(written));
                    g.list->SetPipelineState(GetDepthCopyPipeline(texture.format, true));
                    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    struct
                    {
                        int32_t sourceOffset[2];
                        uint32_t colorIndex;
                        uint32_t floatDepth;
                        uint32_t resolutionScale;
                    } constants{ { 0, (int32_t(sourceTop) - int32_t(destTop)) * int32_t(g_scale) }, other->srv, key.format == 1 ? 1u : 0u,
                        g_scale };
                    g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
                    g.list->DrawInstanced(3, 1, 0, 0);
                    continue;
                }

                Transition(*other, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto rtv = CpuRtv(texture.rtv);
                g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                SetViewportAndScissor(texture.width, texture.height, Scaled(written));
                g.list->SetPipelineState(GetBlitPipeline(texture.format, false, true));
                g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                struct
                {
                    int32_t sourceOffset[2];
                    uint32_t depthIndex;
                    uint32_t stencilIndex;
                    uint32_t floatDepth;
                    int32_t sampleScale[2];
                    uint32_t swapHalves;
                    uint32_t resolutionScale;
                } constants{ { 0, int32_t(sourceTop) - int32_t(destTop) }, other->srv, other->stencilSrv,
                    otherKey->format == 1 ? 1u : 0u, { 1, 1 }, 1u, g_scale };
                g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
                g.list->DrawInstanced(3, 1, 0, 0);
            }
            else if (key.depth)
            {
                Transition(*other, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                auto dsv = CpuDsv(texture.dsv);
                g.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                SetViewportAndScissor(texture.width, texture.height,
                    { 0, LONG(destTop * g_scale), LONG(texture.width), LONG((destTop + height) * g_scale) });
                g.list->SetPipelineState(GetDepthCopyPipeline(texture.format));
                g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                struct
                {
                    int32_t sourceOffset[2];
                    uint32_t depthIndex;
                    uint32_t stencilIndex;
                } constants{ { 0, (int32_t(sourceTop) - int32_t(destTop)) * int32_t(g_scale) }, other->srv, other->stencilSrv };
                g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
                g.list->DrawInstanced(3, 1, 0, 0);
            }
            else
            {
                Transition(*other, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION source{ other->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION dest{ texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_BOX box{ 0, sourceTop * g_scale, 0, texture.width, (sourceTop + height) * g_scale, 1 };
                g.list->CopyTextureRegion(&dest, 0, destTop * g_scale, 0, &source, &box);
            }
        }

        stamp = ++g.renderTargetClock;
    }

    static Texture* GetRenderTarget(uint32_t base, uint32_t pitch, uint32_t format, bool depth)
    {
        RenderTargetKey key{ base, pitch, format, depth };
        auto it = g.renderTargets.find(key);
        if (it != g.renderTargets.end())
        {
            SyncRenderTarget(key, it->second);
            return &it->second;
        }

        DXGI_FORMAT hostFormat = depth ? (format ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT)
            : GetColorTargetFormat(format);
        Texture texture = CreateTexture(pitch * g_scale, RENDER_TARGET_HEIGHT * g_scale, 1, false, hostFormat,
            depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!texture.resource)
            return nullptr;

        LOGF_INFO("render target: EDRAM base {}, pitch {}, format {}{}", base, pitch, format, depth ? " (depth)" : "");
        if (depth)
        {
            g.list->ClearDepthStencilView(CpuDsv(texture.dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        }
        else
        {
            const float black[4] = {};
            g.list->ClearRenderTargetView(CpuRtv(texture.rtv), black, 0, nullptr);
        }

        g.renderTargetOverlapsDirty = true;
        Texture& created = g.renderTargets.emplace(key, std::move(texture)).first->second;
        SyncRenderTarget(key, created);
        return &created;
    }

    static float RegisterFloat(const be<uint32_t>* registers, uint32_t index)
    {
        uint32_t value = registers[index];
        float result;
        memcpy(&result, &value, sizeof(result));
        return result;
    }

    static int32_t SignExtend15(uint32_t value)
    {
        return int32_t(value << 17) >> 17;
    }

    static void GetWindowOffset(const be<uint32_t>* registers, int32_t& x, int32_t& y)
    {
        uint32_t offset = registers[PA_SC_WINDOW_OFFSET];
        x = SignExtend15(offset & 0x7FFF);
        y = SignExtend15((offset >> 16) & 0x7FFF);
    }

    // Window scissor (moved by the window offset unless disabled) intersected with the screen scissor.
    static D3D12_RECT GetScissor(const be<uint32_t>* registers)
    {
        uint32_t tl = registers[PA_SC_WINDOW_SCISSOR_TL], br = registers[PA_SC_WINDOW_SCISSOR_BR];
        int32_t left = tl & 0x7FFF, top = (tl >> 16) & 0x7FFF, right = br & 0x7FFF, bottom = (br >> 16) & 0x7FFF;
        if (!(tl >> 31))
        {
            int32_t x, y;
            GetWindowOffset(registers, x, y);
            left += x;
            right += x;
            top += y;
            bottom += y;
        }

        uint32_t screenTl = registers[PA_SC_SCREEN_SCISSOR_TL], screenBr = registers[PA_SC_SCREEN_SCISSOR_BR];
        left = std::max<int32_t>(left, SignExtend15(screenTl & 0x7FFF));
        top = std::max<int32_t>(top, SignExtend15((screenTl >> 16) & 0x7FFF));
        right = std::min<int32_t>(right, SignExtend15(screenBr & 0x7FFF));
        bottom = std::min<int32_t>(bottom, SignExtend15((screenBr >> 16) & 0x7FFF));
        return { std::max(left, 0), std::max(top, 0), std::max(right, 0), std::max(bottom, 0) };
    }

    // MSAA surfaces are emulated with a sample per pixel: a 2x surface is twice as tall and a 4x one twice as wide
    // and tall, matching how EDRAM lays samples out, so a surface viewed with different sample counts (games clear
    // with 4x and render with 1x) shares one host render target.
    struct SampleScale
    {
        uint32_t x, y;
    };

    static SampleScale GetSampleScale(const be<uint32_t>* registers)
    {
        uint32_t msaa = SurfaceInfo{ registers[RB_SURFACE_INFO] }.MsaaSamples();
        return { msaa >= 2 ? 2u : 1u, msaa >= 1 ? 2u : 1u };
    }

    static D3D12_RECT ScaleRect(D3D12_RECT rect, SampleScale scale)
    {
        return { LONG(rect.left * scale.x), LONG(rect.top * scale.y), LONG(rect.right * scale.x), LONG(rect.bottom * scale.y) };
    }

    // ---- Pipelines ----

    static D3D12_BLEND BlendFactor(uint32_t factor, bool alpha)
    {
        switch (factor)
        {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 6: return D3D12_BLEND_SRC_ALPHA;
        case 7: return D3D12_BLEND_INV_SRC_ALPHA;
        case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 10: return D3D12_BLEND_DEST_ALPHA;
        case 11: return D3D12_BLEND_INV_DEST_ALPHA;
        case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
        case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
        case 16: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
        default: return D3D12_BLEND_ONE;
        }
    }

    static D3D12_BLEND_OP BlendOp(uint32_t op)
    {
        switch (op)
        {
        case 1: return D3D12_BLEND_OP_SUBTRACT;
        case 2: return D3D12_BLEND_OP_MIN;
        case 3: return D3D12_BLEND_OP_MAX;
        case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
        default: return D3D12_BLEND_OP_ADD;
        }
    }

    static D3D12_SHADER_BYTECODE Bytecode(const std::vector<uint8_t>& code)
    {
        return { code.data(), code.size() };
    }

    static ID3D12PipelineState* GetPipeline(const PipelineKey& key, const HostShader& vertexShader, const HostShader* pixelShader,
        bool streamOutput = false)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(key); i++)
            hash = (hash ^ reinterpret_cast<const uint8_t*>(&key)[i]) * 0x100000001B3ull;
        if (streamOutput)
            hash ^= 0x50534F5354524D31ull;

        auto& pipeline = g.pipelines[hash];
        if (pipeline)
            return pipeline.Get();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g.rootSignature.Get();
        desc.VS = Bytecode(*vertexShader.code);
        if (pixelShader)
            desc.PS = Bytecode(*pixelShader->code);
        if (key.geometryShader)
            desc.GS = Bytecode(key.geometryShader == 2 ? g.pointGeometryShader : g.rectangleGeometryShader);

        for (uint32_t i = 0; i < 4; i++)
        {
            auto& target = desc.BlendState.RenderTarget[i];
            uint32_t blend = key.blend[i];
            uint32_t colorSrc = blend & 0x1F, colorOp = (blend >> 5) & 7, colorDst = (blend >> 8) & 0x1F;
            uint32_t alphaSrc = (blend >> 16) & 0x1F, alphaOp = (blend >> 21) & 7, alphaDst = (blend >> 24) & 0x1F;
            target.BlendEnable = !(colorSrc == 1 && colorDst == 0 && colorOp == 0 && alphaSrc == 1 && alphaDst == 0 && alphaOp == 0);
            target.SrcBlend = BlendFactor(colorSrc, false);
            target.DestBlend = BlendFactor(colorDst, false);
            target.BlendOp = BlendOp(colorOp);
            target.SrcBlendAlpha = BlendFactor(alphaSrc, true);
            target.DestBlendAlpha = BlendFactor(alphaDst, true);
            target.BlendOpAlpha = BlendOp(alphaOp);
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = UINT8((key.colorMask >> (i * 4)) & 0xF);
        }
        desc.BlendState.IndependentBlendEnable = TRUE;
        desc.SampleMask = UINT_MAX;

        uint32_t cull = key.rasterizer & 3;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = cull == 1 ? D3D12_CULL_MODE_FRONT : cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        desc.RasterizerState.FrontCounterClockwise = ((key.rasterizer >> 2) & 1) == 0;
        desc.RasterizerState.DepthClipEnable = !((key.rasterizer >> 3) & 1) && !AR_ENV("AR_NO_DEPTH_CLIP");

        uint32_t depth = key.depthControl;
        auto& depthStencil = desc.DepthStencilState;
        depthStencil.DepthEnable = (depth >> 1) & 1;
        depthStencil.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencil.DepthFunc = AR_ENV("AR_NO_DEPTH_TEST") ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC(((depth >> 4) & 7) + 1);
        depthStencil.StencilEnable = depth & 1;
        depthStencil.StencilReadMask = UINT8(key.stencilMasks & 0xFF);
        depthStencil.StencilWriteMask = UINT8((key.stencilMasks >> 8) & 0xFF);
        depthStencil.FrontFace = { D3D12_STENCIL_OP(((depth >> 11) & 7) + 1), D3D12_STENCIL_OP(((depth >> 17) & 7) + 1),
            D3D12_STENCIL_OP(((depth >> 14) & 7) + 1), D3D12_COMPARISON_FUNC(((depth >> 8) & 7) + 1) };
        if ((depth >> 7) & 1)
            depthStencil.BackFace = { D3D12_STENCIL_OP(((depth >> 23) & 7) + 1), D3D12_STENCIL_OP(((depth >> 29) & 7) + 1),
                D3D12_STENCIL_OP(((depth >> 26) & 7) + 1), D3D12_COMPARISON_FUNC(((depth >> 20) & 7) + 1) };
        else
            depthStencil.BackFace = depthStencil.FrontFace;

        desc.IBStripCutValue = key.stripCut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF :
            key.stripCut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topologyType);
        desc.NumRenderTargets = key.renderTargetCount;
        for (uint32_t i = 0; i < key.renderTargetCount; i++)
            desc.RTVFormats[i] = key.renderTargetFormats[i];
        desc.DSVFormat = key.depthFormat;
        desc.SampleDesc.Count = 1;

        // Diagnostics: the position output also streamed out (AR_SHADOW_SO); not kept in the library.
        static const D3D12_SO_DECLARATION_ENTRY soEntry{ 0, "SV_Position", 0, 0, 4, 0 };
        static const UINT soStride = 16;
        if (streamOutput)
        {
            desc.StreamOutput = { &soEntry, 1, &soStride, 1, 0 };
            if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
            {
                LOGF_ERROR("failed to create a stream output pipeline for {:016X}", vertexShader.hash);
                return nullptr;
            }
            return pipeline.Get();
        }

        // Library names include the shaders' bytecode: the same key gives a different pipeline when the translator
        // changes, and a stale entry under the name can't be replaced (it was recompiled on every run).
        uint64_t libraryHash = hash ^ HashMemory(vertexShader.code->data(), vertexShader.code->size()) * 3 ^
            (pixelShader ? HashMemory(pixelShader->code->data(), pixelShader->code->size()) * 5 : 0);
        wchar_t name[17];
        swprintf(name, 17, L"%016llX", (unsigned long long)libraryHash);
        if (g.pipelineLibrary && SUCCEEDED(g.pipelineLibrary->LoadGraphicsPipeline(name, &desc, IID_PPV_ARGS(&pipeline))))
        {
            g.pipelinesLoaded++;
            return pipeline.Get();
        }

        if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
        {
            LOGF_ERROR("failed to create a pipeline for shaders {:016X} / {:016X}", vertexShader.hash, pixelShader ? pixelShader->hash : 0);
            return nullptr;
        }

        g.pipelinesCreated++;
        if (g.pipelineLibrary && SUCCEEDED(g.pipelineLibrary->StorePipeline(name, pipeline.Get())))
            g.pipelineLibraryDirty = true;
        return pipeline.Get();
    }

    static ID3D12PipelineState* GetBlitPipeline(DXGI_FORMAT format, bool present, bool depth)
    {
        auto& pipeline = present ? g.presentPipeline : depth ? g.depthBlitPipelines[format] : g.blitPipelines[format];
        if (pipeline)
            return pipeline.Get();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g.rootSignature.Get();
        desc.VS = Bytecode(g.blitVertexShader);
        desc.PS = Bytecode(present ? g.presentPixelShader : depth ? g.depthBlitPixelShader : g.blitPixelShader);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
        desc.SampleDesc.Count = 1;
        if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
            LOGF_ERROR("failed to create the blit pipeline for format {}", uint32_t(format));

        return pipeline.Get();
    }

    static ID3D12PipelineState* GetDepthCopyPipeline(DXGI_FORMAT format, bool fromColor)
    {
        auto& pipeline = fromColor ? g.colorToDepthPipelines[format] : g.depthCopyPipelines[format];
        if (pipeline)
            return pipeline.Get();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g.rootSignature.Get();
        desc.VS = Bytecode(g.blitVertexShader);
        desc.PS = Bytecode(fromColor ? g.colorToDepthPixelShader : g.depthCopyPixelShader);
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        if (g.stencilRefSupported)
        {
            desc.DepthStencilState.StencilEnable = TRUE;
            desc.DepthStencilState.StencilReadMask = 0xFF;
            desc.DepthStencilState.StencilWriteMask = 0xFF;
            desc.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                D3D12_COMPARISON_FUNC_ALWAYS };
            desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
        }
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.DSVFormat = format;
        desc.SampleDesc.Count = 1;
        if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
            LOGF_ERROR("failed to create the depth copy pipeline for format {}", uint32_t(format));

        return pipeline.Get();
    }

    // ---- Draws ----

    static uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
        case 2: return ByteSwap(value);
        case 3: return (value << 16) | (value >> 16);
        default: return value;
        }
    }

    static void SetViewportAndScissor(uint32_t width, uint32_t height, D3D12_RECT scissor)
    {
        g.drawState.valid = false; // Used by everything recording outside draws.
        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        g.list->RSSetViewports(1, &viewport);
        scissor.right = std::min<LONG>(scissor.right, LONG(width));
        scissor.bottom = std::min<LONG>(scissor.bottom, LONG(height));
        scissor.left = std::min(scissor.left, scissor.right);
        scissor.top = std::min(scissor.top, scissor.bottom);
        g.list->RSSetScissorRects(1, &scissor);
    }

    // Reads the draw's guest indices (or generates them for auto-indexed draws) as 32 bit values.
    static std::vector<uint32_t> ReadIndices(const DrawCall& draw, uint32_t count)
    {
        std::vector<uint32_t> indices(count);
        switch (draw.initiator.Source())
        {
        case SourceSelect::DMA:
        {
            const uint8_t* source = g_memory.Translate<uint8_t>(PhysicalToVirtual(draw.indexBase));
            for (uint32_t i = 0; i < count; i++)
            {
                if (draw.initiator.Index32())
                {
                    uint32_t value;
                    memcpy(&value, source + i * 4, 4);
                    indices[i] = GpuSwap(value, draw.indexEndian);
                }
                else
                {
                    uint16_t value;
                    memcpy(&value, source + i * 2, 2);
                    indices[i] = draw.indexEndian == 1 ? ByteSwap(value) : value;
                }
            }
            break;
        }
        case SourceSelect::Immediate:
            for (uint32_t i = 0; i < count; i++)
            {
                if (draw.initiator.Index32())
                    indices[i] = i < draw.immediateIndices.size() ? draw.immediateIndices[i] : 0;
                else if (i / 2 < draw.immediateIndices.size())
                    indices[i] = (i & 1) ? (draw.immediateIndices[i / 2] & 0xFFFF) : (draw.immediateIndices[i / 2] >> 16);
            }
            break;
        default:
            for (uint32_t i = 0; i < count; i++)
                indices[i] = i;
            break;
        }
        return indices;
    }

    static void Resolve(const be<uint32_t>* registers);

    // Returns the upload buffer offset of a vertex buffer's data: cached across frames while its contents stay the
    // same, copied into the frame's upload space when they keep changing.
    static uint64_t UploadVertexData(uint32_t address, uint32_t size)
    {
        const uint8_t* source = g_memory.Translate<uint8_t>(PhysicalToVirtual(address));
        uint64_t key = (uint64_t(address) << 32) | size;
        auto* entry = &g.vertexCache[key];
        // AR_NO_VERTEX_CACHE: always copied into the frame's upload space.
        static const bool noCache = AR_ENV("AR_NO_VERTEX_CACHE") != nullptr;
        if (!noCache && entry->changes < 3 && size <= VERTEX_CACHE_SIZE / 16)
        {
            if (entry->content.phase == 0)
                entry->content.phase = uint32_t(key % 30) + 1;
            if (!ContentChanged(entry->content, source, size, g.frameNumber, !entry->valid))
                return entry->offset;
            if (entry->valid)
                entry->changes++;

            if (entry->changes < 3)
            {
                uint64_t offset = (g.vertexCacheOffset + 15) & ~15ull;
                if (offset + size > VERTEX_CACHE_BASE + VERTEX_CACHE_SIZE)
                {
                    // Full: emptied before the next draw's uploads (Draw), copied per frame meanwhile.
                    g.vertexCacheFull = true;
                    auto allocation = Upload(size, 16);
                    memcpy(allocation.cpu, source, size);
                    g_profile.vertexBytes += size;
                    return allocation.offset;
                }
                memcpy(g.uploadCpu + offset, source, size);
                g.vertexCacheOffset = offset + size;
                g_profile.vertexBytes += size;
                entry->offset = offset;
                entry->valid = true;
                return offset;
            }
        }

        auto allocation = Upload(size, 16);
        memcpy(allocation.cpu, source, size);
        g_profile.vertexBytes += size;
        return allocation.offset;
    }

    // Logs every draw of one frame, for debugging (frame number from AR_TRACE_FRAME).
    bool TraceThisFrame()
    {
        // AR_TRACE_FRAMES traces that many frames from AR_TRACE_FRAME on.
        static int64_t frame = AR_ENV("AR_TRACE_FRAME") ? atoll(AR_ENV("AR_TRACE_FRAME")) : -1;
        static int64_t count = AR_ENV("AR_TRACE_FRAMES") ? std::max(1ll, atoll(AR_ENV("AR_TRACE_FRAMES"))) : 1;
        return frame >= 0 && int64_t(g.frameNumber) >= frame && int64_t(g.frameNumber) < frame + count;
    }

    void Draw(const be<uint32_t>* registers, const DrawCall& draw, const HostShader& vertexShader, const HostShader* pixelShader)
    {
        if (!Initialize())
            return;

        ScopedTimer timer{ g_profile.draw };
        g_profile.draws++;
        g.occlusionLabel.clear();
        g.soLabel.clear();
        LapTimer setupLap;

        auto mode = EdramMode(registers[RB_MODECONTROL] & 7);
        if (mode == EdramMode::Copy)
        {
            Resolve(registers);
            return;
        }

        if (mode != EdramMode::ColorDepth && mode != EdramMode::DepthOnly)
            return;

        // Depth only mode doesn't run a pixel shader: D3D switches to it when the title sets a null pixel shader, and the
        // previous one stays loaded unused (see EdramMode in Xenia's xenos.h). Running it anyway put shadow casters at the
        // mercy of the last cut-out caster's shader and texture: with no coordinates from the vertex shader, one texel's
        // alpha killed the whole draw or none of it, which changed with the draw order (flickering shadows).
        // AR_DEPTH_ONLY_PS keeps running it.
        static const bool depthOnlyShader = AR_ENV("AR_DEPTH_ONLY_PS") != nullptr;
        if (mode == EdramMode::DepthOnly && !depthOnlyShader)
            pixelShader = nullptr;

        // Debugging (AR_SKIP_PS=<hex hash prefixes, comma separated>): draws with these pixel shaders are skipped.
        static const std::vector<std::string> skippedShaders = []
            {
                std::vector<std::string> list;
                if (const char* text = AR_ENV("AR_SKIP_PS"))
                {
                    std::stringstream stream(text);
                    std::string item;
                    while (std::getline(stream, item, ','))
                        list.push_back(item);
                }
                return list;
            }();
        if (pixelShader && !skippedShaders.empty())
        {
            std::string hash = fmt::format("{:016X}", pixelShader->hash);
            for (const auto& prefix : skippedShaders)
            {
                if (hash.rfind(prefix, 0) == 0)
                    return;
            }
        }
        // Shadows off (--shadows off): draws into the title's shadow map depth buffer (EDRAM base 128, pitch 1040, where
        // its three cascades are rendered) are skipped, except the rectangles that clear it, so the maps stay empty and
        // nothing is in shadow. This also hides the far cascade's flicker (the title adds some casters only on some
        // frames).
        if (!g_videoSettings.shadows && draw.initiator.PrimType() != PrimitiveType::RectangleList &&
            SurfaceInfo{ registers[RB_SURFACE_INFO] }.Pitch() == 1040 && DepthInfo{ registers[RB_DEPTH_INFO] }.Base() == 128 &&
            (registers[RB_DEPTHCONTROL] & 6) != 0)
            return;

        // AR_SKIP_VS: the same for vertex shaders.
        static const std::vector<std::string> skippedVertexShaders = []
            {
                std::vector<std::string> list;
                if (const char* text = AR_ENV("AR_SKIP_VS"))
                {
                    std::stringstream stream(text);
                    std::string item;
                    while (std::getline(stream, item, ','))
                        list.push_back(item);
                }
                return list;
            }();
        if (!skippedVertexShaders.empty())
        {
            std::string hash = fmt::format("{:016X}", vertexShader.hash);
            for (const auto& prefix : skippedVertexShaders)
            {
                if (hash.rfind(prefix, 0) == 0)
                    return;
            }
        }

        if (vertexShader.code == nullptr || vertexShader.code->empty())
        {
            if (TraceThisFrame())
                LOGF_INFO("draw skipped: vertex shader {:016X} did not compile", vertexShader.hash);
            return;
        }
        if (pixelShader && (pixelShader->code == nullptr || pixelShader->code->empty()))
        {
            if (mode == EdramMode::ColorDepth)
            {
                if (TraceThisFrame())
                    LOGF_INFO("draw skipped: pixel shader {:016X} did not compile", pixelShader->hash);
                return;
            }
            pixelShader = nullptr;
        }

        uint32_t count = draw.initiator.NumIndices();
        SampleScale sampleScale = GetSampleScale(registers);
        uint32_t pitch = SurfaceInfo{ registers[RB_SURFACE_INFO] }.Pitch() * sampleScale.x;
        if (count == 0 || pitch == 0)
            return;

        PipelineKey key;
        memset(&key, 0, sizeof(key));
        SystemConstants system{};
        for (auto& transform : system.textureTransforms)
            transform[0] = transform[1] = 1.0f;

        // Render targets. Only the targets the pixel shader writes are bound.
        Texture* colorTargets[4]{};
        uint32_t colorMask = mode == EdramMode::ColorDepth ? uint32_t(registers[RB_COLOR_MASK]) : 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            system.colorExpBias[i] = 1.0f;
            if (!((colorMask >> (i * 4)) & 0xF) || !pixelShader || !pixelShader->shader->writes_color_target(i))
            {
                colorMask &= ~(0xFu << (i * 4));
                continue;
            }

            ColorInfo info{ registers[i == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + i - 1] };
            colorTargets[i] = GetRenderTarget(info.Base(), pitch, info.Format(), false);
            if (!colorTargets[i])
                return;
            ConvertLayout(*colorTargets[i], SurfaceInfo{ registers[RB_SURFACE_INFO] }.MsaaSamples());

            key.renderTargetCount = uint8_t(i + 1);
            key.renderTargetFormats[i] = colorTargets[i]->format;
            system.colorExpBias[i] = std::ldexp(1.0f, info.ExpBias());
            if (info.Format() == 1)
                system.gammaTargets |= 1u << i;
        }

        uint32_t depthControl = registers[RB_DEPTHCONTROL];
        Texture* depthTarget = nullptr;
        if (depthControl & 3)
        {
            DepthInfo info{ registers[RB_DEPTH_INFO] };
            depthTarget = GetRenderTarget(info.Base(), pitch, info.Format(), true);
            if (!depthTarget)
                return;
            ConvertLayout(*depthTarget, SurfaceInfo{ registers[RB_SURFACE_INFO] }.MsaaSamples());
            key.depthFormat = depthTarget->format;
        }
        else
        {
            depthControl = 0;
        }

        if (key.renderTargetCount == 0 && !depthTarget)
        {
            if (TraceThisFrame())
                LOGF_INFO("draw skipped: no render targets (mask {:04X}, depth control {:08X}, mode {}, ps {:016X}, count {})",
                    uint32_t(registers[RB_COLOR_MASK]), uint32_t(registers[RB_DEPTHCONTROL]), uint32_t(mode),
                    pixelShader ? pixelShader->hash : 0, count);
            return;
        }

        // Primitive type, and the conversions for the ones the host has no equivalent of.
        enum class Conversion { None, Fan, Quads, LineLoop } conversion = Conversion::None;
        D3D12_PRIMITIVE_TOPOLOGY topology;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        switch (draw.initiator.PrimType())
        {
        case PrimitiveType::PointList:
            topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            key.geometryShader = g.pointGeometryShader.empty() ? 0 : 2;
            break;
        case PrimitiveType::LineList: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
        case PrimitiveType::LineStrip: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
        case PrimitiveType::LineLoop: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; conversion = Conversion::LineLoop; break;
        case PrimitiveType::TriangleList: case PrimitiveType::TriangleWithWFlags: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case PrimitiveType::TriangleStrip: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case PrimitiveType::TriangleFan: case PrimitiveType::Polygon: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; conversion = Conversion::Fan; break;
        case PrimitiveType::QuadList: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; conversion = Conversion::Quads; break;
        case PrimitiveType::RectangleList: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; key.geometryShader = !g.rectangleGeometryShader.empty(); break;
        default:
        {
            static std::set<uint32_t> warned;
            if (warned.insert(uint32_t(draw.initiator.PrimType())).second)
                LOGF_WARNING("primitive type {} is not supported", uint32_t(draw.initiator.PrimType()));
            return;
        }
        }

        uint32_t modeControl = registers[PA_SU_SC_MODE_CNTL];
        uint32_t cull = topologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !key.geometryShader ? (modeControl & 3) : 0;
        if (cull == 3)
        {
            if (TraceThisFrame())
                LOGF_INFO("draw skipped: both faces culled");
            return;
        }
        key.rasterizer = cull | (((modeControl >> 2) & 1) << 2) | (((registers[PA_CL_CLIP_CNTL] >> 16) & 1) << 3);

        // Textures first: their uploads are recorded as they are made, so they survive an upload flush.
        auto bindTextures = [&](const HostShader& shader)
            {
                for (const auto& binding : shader.shader->texture_bindings())
                {
                    uint32_t index = binding.fetch_constant;
                    TextureFetch fetch;
                    for (uint32_t i = 0; i < 6; i++)
                        fetch.words[i] = registers[SHADER_CONSTANT_FETCH_00_0 + index * 6 + i];

                    uint32_t dimension = uint32_t(binding.fetch_instr.dimension);
                    uint32_t hostDimension = dimension == 2 ? 2 : dimension == 3 ? 3 : 1;
                    // Signedness and gamma apply to fixed point formats; float (16 and 32 bit) and depth formats
                    // ignore them (games set them anyway, for example signed 16_16_16_16_FLOAT light buffers).
                    uint32_t format = fetch.Format();
                    bool fixedPoint = !(format == 22 || format == 23 || (format >= 30 && format <= 32) || (format >= 36 && format <= 38));
                    uint32_t signs = 0;
                    for (uint32_t c = 0; c < 4 && fixedPoint; c++)
                    {
                        uint32_t sign = (fetch.words[0] >> (2 + c * 2)) & 3;
                        if (sign == 1)
                            signs |= 1u << c;
                        else if (sign == 3)
                            signs |= 1u << (c + 4);
                    }

                    auto& slot = system.textures[index];
                    uint32_t rawDepth;
                    slot[0] = GetTexture(fetch, hostDimension, system.textureTransforms[index], rawDepth);
                    if (TraceThisFrame())
                        LOGF_INFO("  texture {} at {:08X} {}x{} format {}", index, fetch.BaseAddress(), fetch.Width(), fetch.Height(), fetch.Format());
                    // Raw depth words can't be filtered.
                    // Filters set in the fetch instruction override the fetch constant's.
                    TextureFetch samplerFetch = fetch;
                    static const bool noFilterOverride = AR_ENV("AR_NO_FILTER_OVERRIDE") != nullptr;
                    const auto& attributes = binding.fetch_instr.attributes;
                    auto overrideField = [&](uint32_t value, uint32_t useFetchConst, uint32_t shift, uint32_t mask)
                        {
                            if (value != useFetchConst && !noFilterOverride)
                                samplerFetch.words[3] = (samplerFetch.words[3] & ~(mask << shift)) | (value << shift);
                        };
                    overrideField(uint32_t(attributes.mag_filter), 3, 19, 3);
                    overrideField(uint32_t(attributes.min_filter), 3, 21, 3);
                    overrideField(uint32_t(attributes.mip_filter), 3, 23, 3);
                    overrideField(uint32_t(attributes.aniso_filter), 7, 25, 7);
                    if (rawDepth & 3)
                        samplerFetch.words[3] &= ~(0xFu << 19);
                    slot[1] = GetSampler(samplerFetch);
                    // Point sampled (magnification and minification): coordinates snap to texel centers in the shader.
                    bool pointSampled = ((samplerFetch.words[3] >> 19) & 3) == 0 && ((samplerFetch.words[3] >> 21) & 3) == 0 &&
                        ((samplerFetch.words[3] >> 25) & 7) < 2;
                    if (pointSampled && !AR_ENV("AR_NO_POINT_SNAP"))
                        rawDepth |= 8;
                    slot[2] = fetch.Swizzle();
                    slot[3] = signs | (rawDepth << 8);
                }
            };
        {
            setupLap.Lap(g_profile.setup);
            DetailTimer textureTimer{ g_profile.textures };
            bindTextures(vertexShader);
            if (pixelShader)
                bindTextures(*pixelShader);
        }
        auto textureEnd = DetailedProfile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        // Index data (buffers reused from draw to draw, to save allocations).
        static thread_local std::vector<uint32_t> indices;
        indices.clear();
        bool indexed = draw.initiator.Source() != SourceSelect::AutoIndex;
        bool primitiveReset = ((modeControl >> 21) & 1) && (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP || topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
        // Strips whose reset index is the host's cut value for the index size use the guest indices as they are.
        bool nativeReset = false;
        if (primitiveReset && conversion == Conversion::None && draw.initiator.Source() == SourceSelect::DMA && !AR_ENV("AR_NO_NATIVE_RESET"))
        {
            uint32_t reset = registers[VGT_MULTI_PRIM_IB_RESET_INDX];
            bool index32 = draw.initiator.Index32();
            if ((!index32 && (reset & 0xFFFF) == 0xFFFF) || (index32 && reset == 0xFFFFFFFF))
            {
                nativeReset = true;
                key.stripCut = index32 ? 2 : 1;
            }
        }
        // Experiment (AR_STRIPS_AS_LISTS): indexed triangle strips drawn as triangle lists, without restarts.
        static const bool stripsAsLists = AR_ENV("AR_STRIPS_AS_LISTS") != nullptr;
        if (stripsAsLists && indexed && topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && conversion == Conversion::None)
        {
            std::vector<uint32_t> source = ReadIndices(draw, count);
            uint32_t reset = primitiveReset ? uint32_t(registers[VGT_MULTI_PRIM_IB_RESET_INDX]) : UINT32_MAX;
            if (primitiveReset && !draw.initiator.Index32())
                reset &= 0xFFFF;
            uint32_t start = 0;
            for (uint32_t i = 0; i <= count; i++)
            {
                if (i < count && !(primitiveReset && source[i] == reset))
                    continue;
                // The strip from start to i: triangle k is (k, k+1, k+2), the first two swapped for odd k.
                for (uint32_t k = start; k + 2 < i; k++)
                {
                    if ((k - start) & 1)
                        indices.insert(indices.end(), { source[k + 1], source[k], source[k + 2] });
                    else
                        indices.insert(indices.end(), { source[k], source[k + 1], source[k + 2] });
                }
                start = i + 1;
            }
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            key.stripCut = 0;
            if (indices.empty())
                return;
        }
        else if (conversion != Conversion::None || (indexed && ((primitiveReset && !nativeReset) || draw.initiator.Source() == SourceSelect::Immediate)))
        {
            std::vector<uint32_t> source = ReadIndices(draw, count);
            if (primitiveReset)
            {
                uint32_t reset = registers[VGT_MULTI_PRIM_IB_RESET_INDX];
                for (auto& index : source)
                    index = index == reset ? UINT32_MAX : index;
                key.stripCut = 2;
            }

            switch (conversion)
            {
            case Conversion::Fan:
                for (uint32_t i = 1; i + 1 < count; i++)
                    indices.insert(indices.end(), { source[0], source[i], source[i + 1] });
                break;
            case Conversion::Quads:
                for (uint32_t i = 0; i + 3 < count; i += 4)
                    indices.insert(indices.end(), { source[i], source[i + 1], source[i + 2], source[i], source[i + 2], source[i + 3] });
                break;
            case Conversion::LineLoop:
                indices = source;
                indices.push_back(source[0]);
                break;
            default:
                indices = std::move(source);
                break;
            }

            indexed = true;
            if (indices.empty())
                return;
        }

        // Vertex data: the part of every vertex buffer the shader fetches from that the draw's vertex indices reach is
        // copied into the upload buffer (games point fetch constants into large shared buffers, the size reaching
        // to their end).
        uint32_t vertexCount = UINT32_MAX;
        {
            uint32_t maxIndex = 0;
            bool known = true;
            if (!indices.empty())
            {
                for (uint32_t value : indices)
                {
                    if (value != UINT32_MAX)
                        maxIndex = std::max(maxIndex, value);
                }
            }
            else if (indexed)
            {
                const uint8_t* source = g_memory.Translate<uint8_t>(PhysicalToVirtual(draw.indexBase));
                bool index32 = draw.initiator.Index32();
                if (!index32 && draw.indexEndian == 1 && (uintptr_t(source) & 1) == 0)
                {
                    // Common case, written so the compiler vectorizes it: 16 bit big endian indices (0xFFFF is the
                    // strip reset, or a real index the guest GPU couldn't reach past anyway).
                    const auto* values = reinterpret_cast<const uint16_t*>(source);
                    uint32_t highest = 0;
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint16_t value = __builtin_bswap16(values[i]);
                        uint32_t candidate = value == 0xFFFF && primitiveReset ? 0u : value;
                        highest = highest > candidate ? highest : candidate;
                    }
                    maxIndex = highest;
                }
                else if (index32 && draw.indexEndian == 2 && (uintptr_t(source) & 3) == 0)
                {
                    const auto* values = reinterpret_cast<const uint32_t*>(source);
                    uint32_t highest = 0;
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint32_t value = __builtin_bswap32(values[i]);
                        uint32_t candidate = value == UINT32_MAX ? 0u : value;
                        highest = highest > candidate ? highest : candidate;
                    }
                    maxIndex = highest;
                }
                else
                {
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint32_t value;
                        if (index32)
                        {
                            memcpy(&value, source + i * 4, 4);
                            value = GpuSwap(value, draw.indexEndian);
                        }
                        else
                        {
                            uint16_t value16;
                            memcpy(&value16, source + i * 2, 2);
                            value = draw.indexEndian == 1 ? ByteSwap(value16) : value16;
                            if (value == 0xFFFF && primitiveReset)
                                continue;
                        }
                        if (value != UINT32_MAX)
                            maxIndex = std::max(maxIndex, value);
                    }
                }
            }
            else
            {
                maxIndex = count ? count - 1 : 0;
            }

            int32_t indexOffset = int32_t(uint32_t(registers[VGT_INDX_OFFSET]));
            static const bool fullBuffers = AR_ENV("AR_FULL_VERTEX_BUFFERS") != nullptr;
            if (known && !fullBuffers && indexOffset >= 0 && maxIndex < 0x1000000)
                vertexCount = maxIndex + uint32_t(indexOffset) + 1;
        }

        uint64_t frameBase = g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT);
        uint64_t uploadSize = 3 * 4096 + indices.size() * 4 + size_t(count) * 4;
        static thread_local std::vector<std::pair<uint32_t, uint32_t>> vertexRanges;
        vertexRanges.clear();
        for (const auto& binding : vertexShader.shader->vertex_bindings())
        {
            uint32_t index = binding.fetch_constant;
            uint32_t size = ((registers[SHADER_CONSTANT_FETCH_00_0 + index * 2 + 1] >> 2) & 0xFFFFFF) * 4;
            if (vertexCount != UINT32_MAX && binding.stride_words != 0)
            {
                uint64_t needed = (uint64_t(vertexCount) * binding.stride_words * 4 + 255) & ~255ull;
                size = uint32_t(std::min<uint64_t>(size, needed));
            }
            vertexRanges.emplace_back(index, size);
            uploadSize += size + 16;
        }

        if (g.uploadOffset + uploadSize > frameBase + UPLOAD_SIZE / FRAME_COUNT || g.vertexCacheFull)
        {
            if (AR_ENV("AR_SHADOW_LOG"))
            {
                std::string ranges;
                for (auto [index, size] : vertexRanges)
                    ranges += fmt::format(" vf{} {:X}", index, size);
                LOGF_INFO("FW {} draw needs {:X} bytes: vs {:016X} prim {} count {} indexed {} vertex count {:X}{}", g.frameNumber, uploadSize,
                    vertexShader.hash, uint32_t(draw.initiator.PrimType()), count, indexed, vertexCount, ranges);
            }
            g_profile.flushes++;
            FlushAndWait();
            if (g.vertexCacheFull)
            {
                // The GPU is done with everything: the vertex cache starts over.
                g.vertexCache.clear();
                g.vertexCacheOffset = VERTEX_CACHE_BASE;
                g.mirrorCacheStart = VERTEX_CACHE_BASE;
                g.vertexCacheFull = false;
            }
        }

        LapTimer lap;
        if (DetailedProfile())
            g_profile.prepare += std::chrono::duration<double, std::milli>(lap.last - textureEnd).count();
        for (auto [index, size] : vertexRanges)
        {
            if (size == 0)
                continue;
            uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
            if (TraceThisFrame())
                LOGF_INFO("  vertex fetch {} at 0x{:08X}: {} bytes, {} indices, {} vertices", index, address, size, count,
                    draw.initiator.Source() == SourceSelect::AutoIndex ? "auto" : "indexed");
            uint64_t& uploaded = g.vertexUploads[(uint64_t(address) << 32) | size];
            // Diagnostics (AR_CHECK_VERTEX_MEMO): uploads reused in the command list whose guest memory has changed.
            static const bool checkMemo = AR_ENV("AR_CHECK_VERTEX_MEMO") != nullptr;
            static std::unordered_map<uint64_t, uint64_t> memoHashes; // Upload offset + 1 -> guest data hash then.
            uint64_t guestHash = checkMemo && size <= 4096 ? HashMemory(g_memory.Translate<uint8_t>(PhysicalToVirtual(address)), size) : 0;
            if (checkMemo && uploaded != 0 && size <= 4096 && memoHashes[uploaded] != guestHash)
            {
                static uint64_t stale;
                bool shadow = depthTarget && DepthInfo{ registers[RB_DEPTH_INFO] }.Base() == 128;
                if (stale++ < 200 || shadow)
                    LOGF_INFO("VM {} stale vertex upload: {:08X} {} bytes, vs {:08X}, depth base {}{}", g.frameNumber, address, size,
                        uint32_t(vertexShader.hash >> 32), depthTarget ? DepthInfo{ registers[RB_DEPTH_INFO] }.Base() : 0, shadow ? " SHADOW" : "");
            }
            if (uploaded == 0)
            {
                uploaded = UploadVertexData(address, size) + 1;
                if (checkMemo)
                    memoHashes[uploaded] = guestHash;
            }
            system.vertexDataOffsets[index] = int32_t(uploaded - 1) - int32_t(address);
            if (CheckUploads() && ((vertexShader.hash >> 32) == 0x741845E2 || (vertexShader.hash >> 32) == 0xDC019066))
                NoteUpload(uploaded - 1, size, "vertices");
        }

        lap.Lap(g_profile.vertices);

        // Constants.
        for (uint32_t i = 0; i < 8; i++)
            system.boolConstants[i] = registers[SHADER_CONSTANT_BOOL_000_031 + i];
        for (uint32_t i = 0; i < 32; i++)
            system.loopConstants[i] = registers[SHADER_CONSTANT_LOOP_00 + i];
        for (uint32_t i = 0; i < 192; i++)
            system.fetchConstants[i] = registers[SHADER_CONSTANT_FETCH_00_0 + i];

        // Guest viewport transform (with the window offset and D3D9 pixel centers) to host clip space.
        uint32_t vte = registers[PA_CL_VTE_CNTL];
        float scaleX = (vte & 1) ? RegisterFloat(registers, PA_CL_VPORT_XSCALE) : 1.0f;
        float offsetX = (vte & 2) ? RegisterFloat(registers, PA_CL_VPORT_XOFFSET) : 0.0f;
        float scaleY = (vte & 4) ? RegisterFloat(registers, PA_CL_VPORT_YSCALE) : 1.0f;
        float offsetY = (vte & 8) ? RegisterFloat(registers, PA_CL_VPORT_YOFFSET) : 0.0f;
        float scaleZ = (vte & 16) ? RegisterFloat(registers, PA_CL_VPORT_ZSCALE) : 1.0f;
        float offsetZ = (vte & 32) ? RegisterFloat(registers, PA_CL_VPORT_ZOFFSET) : 0.0f;
        if ((modeControl >> 16) & 1)
        {
            int32_t windowX, windowY;
            GetWindowOffset(registers, windowX, windowY);
            offsetX += float(windowX);
            offsetY += float(windowY);
        }
        if ((registers[0x2302] & 1) == 0) // PA_SU_VTX_CNTL pixel centers at integers.
        {
            offsetX += 0.5f;
            offsetY += 0.5f;
        }

        float width = float(pitch) / float(sampleScale.x), height = float(RENDER_TARGET_HEIGHT) / float(sampleScale.y);
        system.ndcScale[0] = scaleX * 2.0f / width;
        system.ndcScale[1] = -scaleY * 2.0f / height;
        system.ndcScale[2] = scaleZ;
        system.ndcOffset[0] = offsetX * 2.0f / width - 1.0f;
        system.ndcOffset[1] = 1.0f - offsetY * 2.0f / height;
        system.ndcOffset[2] = offsetZ;
        system.flags = ((vte >> 8) & 1) | (((vte >> 9) & 1) << 1) | (((vte >> 10) & 1) << 2);
        // Debugging (AR_HIGHLIGHT_PRIM=<primitive type>): those draws output solid magenta.
        static const int highlightPrimitive = AR_ENV("AR_HIGHLIGHT_PRIM") ? atoi(AR_ENV("AR_HIGHLIGHT_PRIM")) : -1;
        if (int(draw.initiator.PrimType()) == highlightPrimitive)
            system.flags |= 16;
        // AR_HIGHLIGHT_PS=<hash prefix>: draws with that pixel shader output solid magenta.
        static const char* highlightShader = AR_ENV("AR_HIGHLIGHT_PS");
        if (highlightShader && pixelShader && fmt::format("{:016X}", pixelShader->hash).rfind(highlightShader, 0) == 0)
            system.flags |= 16;
        if (draw.initiator.PrimType() == PrimitiveType::PointList)
        {
            // PA_SU_POINT_SIZE and PA_SU_POINT_MINMAX hold radii in 12.4 fixed point.
            uint32_t pointSize = registers[PA_SU_POINT_SIZE], pointMinMax = registers[PA_SU_POINT_MINMAX];
            system.flags |= 8;
            system.point[0] = float(pointSize >> 16) / 8.0f;
            system.point[1] = float(pointSize & 0xFFFF) / 8.0f;
            system.point[2] = float(pointMinMax & 0xFFFF) / 8.0f;
            system.point[3] = float(pointMinMax >> 16) / 8.0f;
            system.pointNdc[0] = 1.0f / width;
            system.pointNdc[1] = 1.0f / height;
        }
        system.vertexIndexOffset = int32_t(uint32_t(registers[VGT_INDX_OFFSET]));

        uint32_t programControl = registers[SQ_PROGRAM_CNTL];
        system.paramGenRegister = ((programControl >> 18) & 1) ? int32_t((registers[SQ_CONTEXT_MISC] >> 8) & 0xFF) : -1;
        uint32_t colorControl = registers[RB_COLORCONTROL];
        system.alphaFunc = ((colorControl >> 3) & 1) ? (colorControl & 7) : 7;
        system.alphaRef = RegisterFloat(registers, RB_ALPHA_REF);
        system.resolutionScale = float(g_scale);

        // Only the constants up to the highest one the shader reads are copied (all of them when it indexes them).
        auto uploadFloatConstants = [&](uint32_t baseRegister, const HostShader* shader)
            {
                uint32_t count = 0;
                if (shader)
                {
                    const auto& map = shader->shader->constant_register_map();
                    static const bool fullConstants = AR_ENV("AR_FULL_CONSTANTS") != nullptr;
                    if (map.float_dynamic_addressing || fullConstants)
                        count = 256;
                    for (uint32_t i = 0; i < 4; i++)
                    {
                        if (map.float_bitmap[i])
                            count = std::max<uint32_t>(count, i * 64 + 64 - uint32_t(std::countl_zero(map.float_bitmap[i])));
                    }
                }

                uint32_t base = std::min<uint32_t>(baseRegister & 0x1FF, 256);
                // Only the registers read are uploaded; the shader declares all 256, so reads past them land in
                // later upload space (or the buffer's padding), which it never uses.
                auto allocation = Upload(std::max<uint32_t>(count * 16, 256), 256);
                auto* out = reinterpret_cast<uint32_t*>(allocation.cpu);
                const auto* in = reinterpret_cast<const uint32_t*>(&registers[SHADER_CONSTANT_000_X + base * 4]);
                for (uint32_t i = 0; i < count * 4; i++)
                    out[i] = __builtin_bswap32(in[i]);
                return allocation.gpu;
            };
        D3D12_GPU_VIRTUAL_ADDRESS vertexConstants = uploadFloatConstants(registers[0x2307], &vertexShader);
        D3D12_GPU_VIRTUAL_ADDRESS pixelConstants = uploadFloatConstants(registers[0x2308], pixelShader);
        D3D12_GPU_VIRTUAL_ADDRESS systemConstants = UploadConstants(system);
        if (CheckUploads() && ((vertexShader.hash >> 32) == 0x741845E2 || (vertexShader.hash >> 32) == 0xDC019066))
        {
            NoteUpload(vertexConstants - g.uploadGpu, 32 * 16, "vertex constants");
            NoteUpload(systemConstants - g.uploadGpu, sizeof(system), "system constants");
        }

        lap.Lap(g_profile.constants);

        // Diagnostics (AR_SHADOW_LOG with AR_DUMP_FRAME/AR_DUMP_FRAMES): one short line per draw into the shadow map
        // depth buffer (EDRAM base 128, pitch 1040), cheap enough to keep the game at full speed.
        static const bool shadowLog = AR_ENV("AR_SHADOW_LOG") != nullptr;
        if (shadowLog && depthTarget && SurfaceInfo{ registers[RB_SURFACE_INFO] }.Pitch() == 1040 &&
            DepthInfo{ registers[RB_DEPTH_INFO] }.Base() == 128)
        {
            static const int64_t first = AR_ENV("AR_DUMP_FRAME") ? atoll(AR_ENV("AR_DUMP_FRAME")) : 0;
            static const int64_t frames = AR_ENV("AR_DUMP_FRAMES") ? atoll(AR_ENV("AR_DUMP_FRAMES")) : 1;
            if (int64_t(g.frameNumber) >= first && int64_t(g.frameNumber) < first + frames)
            {
                uint32_t vsBase = std::min<uint32_t>(registers[0x2307] & 0x1FF, 256);
                const auto* c = reinterpret_cast<const uint8_t*>(&registers[SHADER_CONSTANT_000_X + vsBase * 4]);
                uint32_t address = vertexRanges.empty() ? 0 : registers[SHADER_CONSTANT_FETCH_00_0 + vertexRanges[0].first * 2] & 0x1FFFFFFC;
                // Then all context registers (render state, viewport, scissor...) and the constants derived for the
                // host shaders, to find state that differs between frames although the draw is the same.
                LOGF_INFO("S {} {:08X} {:08X} {} {} {:016X} {:016X} {:016X} {:016X}", g.frameNumber, uint32_t(vertexShader.hash >> 32), address,
                    int32_t(uint32_t(registers[VGT_INDX_OFFSET])), count, HashMemory(c + 16 * 16, 4 * 16), HashMemory(c + 28 * 16, 4 * 16),
                    HashMemory(reinterpret_cast<const uint8_t*>(&registers[0x2000]), 0x400 * 4),
                    HashMemory(reinterpret_cast<const uint8_t*>(&system), sizeof(system)));
                uint32_t ibAddresses[4], ibSizes[4];
                uint32_t ibCount = GetIndirectBufferChain(ibAddresses, ibSizes, 4);
                std::string chain;
                for (uint32_t i = 0; i < ibCount; i++)
                    chain += fmt::format(" {:08X}:{}", ibAddresses[i], ibSizes[i]);
                LOGF_INFO("IB {} {:08X}{} marker {:08X} {:08X}", g.frameNumber, address, chain, CurrentDrawMarker(),
                    CurrentDrawMarkerObject());
                if (g.occlusionHeap)
                {
                    // Then the vertex data: size, where the GPU reads it, and hashes of guest memory and of that copy.
                    // AR_SHADOW_PIXELS=2: with hashes of what the draws read (slow).
                    static const bool detailed = atoi(AR_ENV("AR_SHADOW_PIXELS")) >= 2 && atoi(AR_ENV("AR_SHADOW_PIXELS")) <= 4;
                    std::string vertexText;
                    if (detailed && !vertexRanges.empty() && g.shadowResolves == 0)
                    {
                        auto [index, fullSize] = vertexRanges[0];
                        uint32_t size = std::min<uint32_t>(fullSize, 65536);
                        uint32_t uploaded = address + uint32_t(system.vertexDataOffsets[index]);
                        vertexText = fmt::format(" v {} {:X} {:016X} {:016X}", size, uploaded,
                            HashMemory(g_memory.Translate<uint8_t>(PhysicalToVirtual(address)), size), HashMemory(g.uploadCpu + uploaded, size));
                    }
                    g.occlusionLabel = fmt::format("{} {} {:08X} {:08X} {} {}{}", g.frameNumber, g.shadowResolves,
                        uint32_t(vertexShader.hash >> 32), address, int32_t(uint32_t(registers[VGT_INDX_OFFSET])), count, vertexText);
                    static const bool logStats = AR_ENV("AR_SHADOW_STATS") != nullptr;
                    if (logStats)
                        g.occlusionLabel += fmt::format(" ps {:08X} cc {:08X} aref {} dc {:08X}", pixelShader ? uint32_t(pixelShader->hash >> 32) : 0u,
                            uint32_t(registers[RB_COLORCONTROL]), RegisterFloat(registers, RB_ALPHA_REF), uint32_t(registers[RB_DEPTHCONTROL]));
                    if (logStats)
                    {
                        // What the cut-out pixel shader (4018513F) reads: texture 0, and the constants it adds to the
                        // coordinates and compares the alpha with.
                        TextureFetch fetch0;
                        for (uint32_t i = 0; i < 6; i++)
                            fetch0.words[i] = registers[SHADER_CONSTANT_FETCH_00_0 + i];
                        uint32_t psBase = uint32_t(registers[0x2308]) & 0x1FF;
                        auto pixelConstant = [&](uint32_t c, uint32_t component)
                            {
                                return psBase + c < 512 ? RegisterFloat(registers, SHADER_CONSTANT_000_X + (psBase + c) * 4 + component) : -999.0f;
                            };
                        g.occlusionLabel += TakeFetch0Log();
                        g.occlusionLabel += fmt::format(" mode {:X}", uint32_t(registers[RB_MODECONTROL]) & 7);
                        g.occlusionLabel += fmt::format(" tf0 {:08X} {}x{} fmt {} srv {} xf {} {} raw {:X} c185 {} {} c255 {} {}", fetch0.BaseAddress(), fetch0.Width(),
                            fetch0.Height(), fetch0.Format(), system.textures[0][0], system.textureTransforms[0][0], system.textureTransforms[0][1],
                            system.textures[0][3], pixelConstant(185, 0), pixelConstant(185, 1), pixelConstant(255, 0), pixelConstant(255, 1));
                    }

                    // AR_SHADOW_CPU_VS: the far cascade's static caster shader (741845E2) run on the CPU for the draw's
                    // indices: vertices inside the shadow map, out of all, and the depth range.
                    static const bool cpuVs = AR_ENV("AR_SHADOW_CPU_VS") != nullptr;
                    if (cpuVs && g.shadowResolves == 0 && (vertexShader.hash >> 32) == 0x741845E2 && indexed &&
                        !draw.initiator.Index32() && draw.indexEndian == 1)
                    {
                        auto constant = [&](uint32_t c, uint32_t component) { return RegisterFloat(registers, SHADER_CONSTANT_000_X + (vsBase + c) * 4 + component); };
                        auto dot = [&](const float* v, uint32_t c) { return v[0] * constant(c, 0) + v[1] * constant(c, 1) + v[2] * constant(c, 2) + v[3] * constant(c, 3); };
                        const uint8_t* indexData = g_memory.Translate<uint8_t>(PhysicalToVirtual(draw.indexBase));
                        const uint8_t* vertexData = g_memory.Translate<uint8_t>(PhysicalToVirtual(address));
                        int32_t indexOffset = int32_t(uint32_t(registers[VGT_INDX_OFFSET]));
                        uint32_t inside = 0, total = 0, bad = 0;
                        float zLow = FLT_MAX, zHigh = -FLT_MAX;
                        for (uint32_t i = 0; i < count; i++)
                        {
                            uint16_t value;
                            memcpy(&value, indexData + i * 2, 2);
                            value = ByteSwap(value);
                            if (value == 0xFFFF)
                                continue;
                            uint32_t vertex = uint32_t(int32_t(value) + indexOffset);
                            float p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                            for (int c = 0; c < 3; c++)
                            {
                                uint32_t bits;
                                memcpy(&bits, vertexData + vertex * 32 + c * 4, 4);
                                bits = ByteSwap(bits);
                                memcpy(&p[c], &bits, 4);
                            }
                            float r0[4] = { dot(p, 19), dot(p, 18), dot(p, 16), dot(p, 17) };
                            // dp4 with .wzxy on both sides of the light matrix rows.
                            auto row = [&](uint32_t c) { return r0[0] * constant(c, 3) + r0[1] * constant(c, 2) + r0[2] * constant(c, 0) + r0[3] * constant(c, 1); };
                            float x = row(28), y = row(29), z = row(30), w = row(31);
                            total++;
                            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w) || w == 0.0f)
                            {
                                bad++;
                                continue;
                            }
                            x /= w;
                            y /= w;
                            z /= w;
                            zLow = std::min(zLow, z);
                            zHigh = std::max(zHigh, z);
                            inside += std::abs(x) <= 1.0f && std::abs(y) <= 1.0f && z >= 0.0f && z <= 1.0f;
                        }
                        g.occlusionLabel += fmt::format(" cpu {}/{} bad {} z {:.3f}..{:.3f}", inside, total, bad, zLow, zHigh);
                    }

                    static const std::string soOf = AR_ENV("AR_SHADOW_SO") ? AR_ENV("AR_SHADOW_SO") : "";
                    if (g.soBuffer && g.shadowResolves == 0 && soOf.find(fmt::format("{:08X}", address)) != std::string::npos)
                        g.soLabel = g.occlusionLabel;

                    // AR_SHADOW_CONSTANTS=<vertex buffers>: the float constants of those buffers' first draws (PC lines).
                    static const std::string constantsOf = AR_ENV("AR_SHADOW_CONSTANTS") ? AR_ENV("AR_SHADOW_CONSTANTS") : "";
                    if (!constantsOf.empty() && g.shadowResolves == 0 &&
                        constantsOf.find(fmt::format("{:08X}", address)) != std::string::npos)
                    {
                        std::string text;
                        for (uint32_t i = 0; i < 64 * 4; i++)
                            text += fmt::format(" {}", RegisterFloat(registers, SHADER_CONSTANT_000_X + vsBase * 4 + i));
                        const auto& map = vertexShader.shader->constant_register_map();
                        LOGF_INFO("PC {} {:08X} base {} reads {:016X} {:016X} {:016X} {:016X}{}:{}", g.frameNumber, address, vsBase,
                            map.float_bitmap[0], map.float_bitmap[1], map.float_bitmap[2], map.float_bitmap[3],
                            map.float_dynamic_addressing ? " dynamic" : "", text);
                        std::string registerText;
                        for (uint32_t i = 0; i < 0x400; i++)
                            registerText += fmt::format("{:08X} ", uint32_t(registers[0x2000 + i]));
                        LOGF_INFO("PR {} {:08X} {} {}", g.frameNumber, address, int32_t(uint32_t(registers[VGT_INDX_OFFSET])), registerText);
                        std::string systemText;
                        for (uint32_t i = 0; i < sizeof(system) / 4; i++)
                            systemText += fmt::format("{:08X} ", reinterpret_cast<const uint32_t*>(&system)[i]);
                        LOGF_INFO("PY {} {:08X} {} {}", g.frameNumber, address, int32_t(uint32_t(registers[VGT_INDX_OFFSET])), systemText);
                    }
                }

                // The full context registers once a frame, at the first static geometry draw into the shadow map.
                static uint64_t dumpedFrame = UINT64_MAX;
                if (dumpedFrame != g.frameNumber && (vertexShader.hash >> 32) == 0x741845E2)
                {
                    dumpedFrame = g.frameNumber;
                    std::string text;
                    text.reserve(0x400 * 9);
                    for (uint32_t i = 0; i < 0x400; i++)
                        text += fmt::format("{:08X} ", uint32_t(registers[0x2000 + i]));
                    LOGF_INFO("R {} {}", g.frameNumber, text);
                }
                static uint64_t lastLight;
                uint64_t light = HashMemory(c + 28 * 16, 4 * 16);
                if (light != lastLight)
                {
                    lastLight = light;
                    float m[16];
                    for (uint32_t i = 0; i < 16; i++)
                        m[i] = RegisterFloat(registers, SHADER_CONSTANT_000_X + (vsBase + 28) * 4 + i);
                    LOGF_INFO("L {} {:016X} {} {} {} {} | {} {} {} {} | {} {} {} {} | {} {} {} {} t {}", g.frameNumber, light, m[0], m[1], m[2], m[3],
                        m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], uint32_t(GetTickCount() & 0x7FFFFFFF));
                }
            }
        }

        // Diagnostics (AR_TRACE_INPUTS with AR_TRACE_FRAME): hashes of what each draw reads, to compare frames.
        static const bool traceInputs = AR_ENV("AR_TRACE_INPUTS") != nullptr;
        if (traceInputs && TraceThisFrame())
        {
            uint64_t vertexHash = 0;
            for (auto [index, size] : vertexRanges)
            {
                uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
                vertexHash ^= HashMemory(g_memory.Translate<uint8_t>(PhysicalToVirtual(address)), size) + index;
            }
            uint32_t vsBase = std::min<uint32_t>(registers[0x2307] & 0x1FF, 256);
            uint64_t constantHash = HashMemory(reinterpret_cast<const uint8_t*>(&registers[SHADER_CONSTANT_000_X + vsBase * 4]), 64 * 16);
            LOGF_INFO("  inputs vertices {:016X} indices {:016X} vs constants {:016X} index offset {} max index {}", vertexHash,
                indices.empty() ? 0 : HashMemory(reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * 4), constantHash,
                int32_t(uint32_t(registers[VGT_INDX_OFFSET])), vertexCount);
        }
        D3D12_INDEX_BUFFER_VIEW indexView{};
        if (indexed && !indices.empty())
        {
            auto allocation = Upload(indices.size() * 4, 16);
            memcpy(allocation.cpu, indices.data(), indices.size() * 4);
            indexView = { allocation.gpu, UINT(indices.size() * 4), DXGI_FORMAT_R32_UINT };
            count = uint32_t(indices.size());
        }
        else if (indexed)
        {
            // Guest index buffers are copied and byte swapped.
            bool index32 = draw.initiator.Index32();
            uint32_t size = count * (index32 ? 4 : 2);
            auto allocation = Upload((size + 3) & ~3u, 16);
            const uint8_t* source = g_memory.Translate<uint8_t>(PhysicalToVirtual(draw.indexBase));
            if (index32)
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t value;
                    memcpy(&value, source + i * 4, 4);
                    value = GpuSwap(value, draw.indexEndian);
                    memcpy(allocation.cpu + i * 4, &value, 4);
                }
            }
            else
            {
                if (draw.indexEndian == 1 && (uintptr_t(source) & 1) == 0)
                {
                    const auto* in = reinterpret_cast<const uint16_t*>(source);
                    auto* out = reinterpret_cast<uint16_t*>(allocation.cpu);
                    for (uint32_t i = 0; i < count; i++)
                        out[i] = __builtin_bswap16(in[i]);
                }
                else
                {
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint16_t value;
                        memcpy(&value, source + i * 2, 2);
                        value = draw.indexEndian == 1 ? ByteSwap(value) : value;
                        memcpy(allocation.cpu + i * 2, &value, 2);
                    }
                }
            }
            indexView = { allocation.gpu, size, index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
            if (CheckUploads() && ((vertexShader.hash >> 32) == 0x741845E2 || (vertexShader.hash >> 32) == 0xDC019066))
                NoteUpload(allocation.offset, size, "indices");
        }

        lap.Lap(g_profile.indices);

        // Pipeline state.
        key.vertexShader = vertexShader.hash;
        key.pixelShader = pixelShader ? pixelShader->hash : 0;
        static constexpr uint32_t blendRegisters[] = { RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 };
        for (uint32_t i = 0; i < 4; i++)
            key.blend[i] = i < key.renderTargetCount ? (registers[blendRegisters[i]] & 0x1FFF1FFF) : 0x00010001;
        key.colorMask = colorMask;
        key.depthControl = depthControl;
        uint32_t stencilRefMask = registers[RB_STENCILREFMASK];
        key.stencilMasks = (stencilRefMask >> 8) & 0xFFFF;
        key.topologyType = uint8_t(topologyType);

        ID3D12PipelineState* pipeline = GetPipeline(key, vertexShader, pixelShader);
        if (!pipeline)
        {
            if (TraceThisFrame())
                LOGF_INFO("draw skipped: no pipeline for vs {:016X} ps {:016X}", key.vertexShader, key.pixelShader);
            return;
        }

        if (TraceThisFrame())
        {
            D3D12_RECT scissor = GetScissor(registers);
            std::string textures;
            for (uint32_t i = 0; i < 32; i++)
            {
                if (system.textures[i][0] > 2)
                {
                    TextureFetch fetch;
                    for (uint32_t w = 0; w < 6; w++)
                        fetch.words[w] = registers[SHADER_CONSTANT_FETCH_00_0 + i * 6 + w];
                    textures += fmt::format(" tf{}=srv{}@{:08X}/fmt{}/{}x{}", i, system.textures[i][0],
                        registers[SHADER_CONSTANT_FETCH_00_0 + i * 6 + 1] & 0xFFFFF000, registers[SHADER_CONSTANT_FETCH_00_0 + i * 6 + 1] & 0x3F,
                        fetch.Width(), fetch.Height());
                    if (AR_ENV("AR_TRACE_FETCH"))
                        textures += fmt::format("[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", fetch.words[0], fetch.words[1],
                            fetch.words[2], fetch.words[3], fetch.words[4], fetch.words[5]);
                }
            }
            LOGF_INFO("draw prim {} count {} indexed {} vs {:016X} ps {:016X} rt {} mask {:04X} depth {:08X} pitch {} scissor {},{}-{},{} ndc {:.4f},{:.4f},{:.4f} / {:.4f},{:.4f},{:.4f} vte {:X} blend {:08X}{}",
                uint32_t(draw.initiator.PrimType()), count, indexed, key.vertexShader, key.pixelShader, key.renderTargetCount,
                colorMask, depthControl, pitch, scissor.left, scissor.top, scissor.right, scissor.bottom,
                system.ndcScale[0], system.ndcScale[1], system.ndcScale[2], system.ndcOffset[0], system.ndcOffset[1], system.ndcOffset[2],
                vte, key.blend[0], textures);
            if (CurrentDrawMarker() != 0)
                LOGF_INFO("  marker {:08X} model {:08X}", CurrentDrawMarker(), CurrentDrawMarkerObject());
            // AR_TRACE_PS=<hash prefix>: the float constants that draw's shaders read.
            if (const char* tracePs = AR_ENV("AR_TRACE_PS"); tracePs && pixelShader &&
                fmt::format("{:016X}", pixelShader->hash).rfind(tracePs, 0) == 0)
            {
                LOGF_INFO("  loop constants 31 {:08X} 0 {:08X}, bool {:08X} {:08X}", uint32_t(registers[SHADER_CONSTANT_LOOP_00 + 31]),
                    uint32_t(registers[SHADER_CONSTANT_LOOP_00]), uint32_t(registers[SHADER_CONSTANT_BOOL_000_031]),
                    uint32_t(registers[SHADER_CONSTANT_BOOL_000_031 + 4]));
                for (int stage = 0; stage < 2; stage++)
                {
                    const HostShader* shader = stage == 0 ? &vertexShader : pixelShader;
                    uint32_t base = std::min<uint32_t>(registers[stage == 0 ? 0x2307 : 0x2308] & 0x1FF, 256);
                    const auto& map = shader->shader->constant_register_map();
                    for (uint32_t i = 0; i < 256; i++)
                    {
                        if (!((map.float_bitmap[i / 64] >> (i % 64)) & 1))
                            continue;
                        float c[4];
                        for (uint32_t k = 0; k < 4; k++)
                            c[k] = RegisterFloat(registers, SHADER_CONSTANT_000_X + (base + i) * 4 + k);
                        LOGF_INFO("  {} c{} = {} {} {} {}", stage == 0 ? "vs" : "ps", i, c[0], c[1], c[2], c[3]);
                    }
                }
            }
            LOGF_INFO("  colorcontrol {:08X} clip {:08X}", uint32_t(registers[RB_COLORCONTROL]), uint32_t(registers[PA_CL_CLIP_CNTL]));
            LOGF_INFO("  line {:08X} point {:08X} {:08X}", uint32_t(registers[0x2282]), uint32_t(registers[PA_SU_POINT_SIZE]),
                uint32_t(registers[PA_SU_POINT_MINMAX]));
            LOGF_INFO("  surface {:08X} color {:08X} depth {:08X} window {:08X} vport {} {} {} {}", uint32_t(registers[RB_SURFACE_INFO]),
                uint32_t(registers[RB_COLOR_INFO]), uint32_t(registers[RB_DEPTH_INFO]), uint32_t(registers[PA_SC_WINDOW_OFFSET]),
                RegisterFloat(registers, PA_CL_VPORT_XSCALE), RegisterFloat(registers, PA_CL_VPORT_XOFFSET),
                RegisterFloat(registers, PA_CL_VPORT_YSCALE), RegisterFloat(registers, PA_CL_VPORT_YOFFSET));
        }

        lap.Lap(g_profile.pipeline);

        // Regions written, for render targets sharing EDRAM. Clears drawn as a rectangle in screen coordinates
        // cover their rectangle, not the scissor (which is often the whole 8192x8192 space).
        {
            D3D12_RECT written = ScaleRect(GetScissor(registers), sampleScale);
            written.right = std::min<LONG>(written.right, LONG(pitch));
            written.bottom = std::min<LONG>(written.bottom, LONG(RENDER_TARGET_HEIGHT));
            const auto& bindings = vertexShader.shader->vertex_bindings();
            if (draw.initiator.PrimType() == PrimitiveType::RectangleList && draw.initiator.NumIndices() == 3 &&
                draw.initiator.Source() == SourceSelect::AutoIndex && (vte & 0x3F) == 0 && !bindings.empty())
            {
                uint32_t fetchIndex = bindings[0].fetch_constant;
                uint32_t fetch0 = registers[SHADER_CONSTANT_FETCH_00_0 + fetchIndex * 2];
                uint32_t fetch1 = registers[SHADER_CONSTANT_FETCH_00_0 + fetchIndex * 2 + 1];
                const uint8_t* data = g_memory.Translate<uint8_t>(PhysicalToVirtual(fetch0 & 0x1FFFFFFC));
                float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
                for (uint32_t v = 0; v < 3; v++)
                {
                    float position[2];
                    for (uint32_t c = 0; c < 2; c++)
                    {
                        uint32_t raw;
                        memcpy(&raw, data + v * bindings[0].stride_words * 4 + c * 4, 4);
                        raw = GpuSwap(raw, fetch1 & 3);
                        memcpy(&position[c], &raw, 4);
                    }
                    minX = std::min(minX, position[0]);
                    maxX = std::max(maxX, position[0]);
                    minY = std::min(minY, position[1]);
                    maxY = std::max(maxY, position[1]);
                }
                int32_t windowX = 0, windowY = 0;
                if ((modeControl >> 16) & 1)
                    GetWindowOffset(registers, windowX, windowY);
                if (minX > -16384.0f && maxX < 16384.0f && minY > -16384.0f && maxY < 16384.0f)
                {
                    D3D12_RECT bounds{ LONG(std::floor(minX)) + windowX, LONG(std::floor(minY)) + windowY,
                        LONG(std::ceil(maxX)) + windowX, LONG(std::ceil(maxY)) + windowY };
                    bounds = ScaleRect(bounds, sampleScale);
                    written = { std::max(written.left, bounds.left), std::max(written.top, bounds.top),
                        std::min(written.right, bounds.right), std::min(written.bottom, bounds.bottom) };
                }
                if (TraceThisFrame())
                    LOGF_INFO("  clear rectangle {},{}-{},{}", minX, minY, maxX, maxY);
                static const bool shadowLog = AR_ENV("AR_SHADOW_LOG") != nullptr;
                if (shadowLog && depthTarget && DepthInfo{ registers[RB_DEPTH_INFO] }.Base() == 128)
                {
                    static const int64_t first = AR_ENV("AR_DUMP_FRAME") ? atoll(AR_ENV("AR_DUMP_FRAME")) : 0;
                    static const int64_t frames = AR_ENV("AR_DUMP_FRAMES") ? atoll(AR_ENV("AR_DUMP_FRAMES")) : 1;
                    if (int64_t(g.frameNumber) >= first && int64_t(g.frameNumber) < first + frames)
                        LOGF_INFO("SC {} {},{}-{},{} surface {:08X} depth {:08X} control {:08X} written {},{}-{},{} layout {}", g.frameNumber,
                            minX, minY, maxX, maxY, uint32_t(registers[RB_SURFACE_INFO]), uint32_t(registers[RB_DEPTH_INFO]),
                            depthControl, written.left, written.top, written.right, written.bottom, depthTarget->layout);
                }
            }

            for (uint32_t i = 0; i < key.renderTargetCount; i++)
            {
                if (colorTargets[i])
                {
                    ColorInfo info{ registers[i == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + i - 1] };
                    RecordWrite({ info.Base(), pitch, info.Format(), false }, written);
                }
            }
            if (depthTarget && (depthControl & 5))
            {
                DepthInfo info{ registers[RB_DEPTH_INFO] };
                RecordWrite({ info.Base(), pitch, info.Format(), true }, written);
            }
        }

        lap.Lap(g_profile.targets);

        // Record.
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4]{};
        for (uint32_t i = 0; i < key.renderTargetCount; i++)
        {
            if (colorTargets[i])
            {
                Transition(*colorTargets[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
                rtvs[i] = CpuRtv(colorTargets[i]->rtv);
            }
            else
            {
                rtvs[i] = CpuRtv(g.nullRtv);
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (depthTarget)
        {
            Transition(*depthTarget, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            dsv = CpuDsv(depthTarget->dsv);
        }
        auto& state = g.drawState;
        static const bool noStateCache = AR_ENV("AR_NO_STATE_CACHE") != nullptr;
        bool known = state.valid && !noStateCache;
        if (!known || state.renderTargetCount != key.renderTargetCount || memcmp(state.rtvs, rtvs, sizeof(rtvs)) != 0 ||
            state.dsv.ptr != dsv.ptr)
        {
            g.list->OMSetRenderTargets(key.renderTargetCount, rtvs, FALSE, depthTarget ? &dsv : nullptr);
            state.renderTargetCount = key.renderTargetCount;
            memcpy(state.rtvs, rtvs, sizeof(rtvs));
            state.dsv = dsv;
        }

        uint32_t viewportWidth = pitch * g_scale, viewportHeight = RENDER_TARGET_HEIGHT * g_scale;
        D3D12_RECT scissor = Scaled(ScaleRect(GetScissor(registers), sampleScale));
        scissor.right = std::min<LONG>(scissor.right, LONG(viewportWidth));
        scissor.bottom = std::min<LONG>(scissor.bottom, LONG(viewportHeight));
        scissor.left = std::min(scissor.left, scissor.right);
        scissor.top = std::min(scissor.top, scissor.bottom);
        if (!known || state.viewportWidth != viewportWidth || state.viewportHeight != viewportHeight)
        {
            D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(viewportWidth), float(viewportHeight), 0.0f, 1.0f };
            g.list->RSSetViewports(1, &viewport);
            state.viewportWidth = viewportWidth;
            state.viewportHeight = viewportHeight;
        }
        if (!known || memcmp(&state.scissor, &scissor, sizeof(scissor)) != 0)
        {
            g.list->RSSetScissorRects(1, &scissor);
            state.scissor = scissor;
        }

        if (!known || state.pipeline != pipeline)
        {
            g.list->SetPipelineState(pipeline);
            state.pipeline = pipeline;
        }
        if (!known || state.topology != topology)
        {
            g.list->IASetPrimitiveTopology(topology);
            state.topology = topology;
        }
        g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, systemConstants);
        g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, vertexConstants);
        g.list->SetGraphicsRootConstantBufferView(ROOT_PIXEL_FLOAT_CONSTANTS, pixelConstants);
        if (!known)
            g.list->SetGraphicsRootShaderResourceView(ROOT_VERTEX_DATA, g.uploadGpu);

        float blendFactor[4] = { RegisterFloat(registers, RB_BLEND_RED), RegisterFloat(registers, RB_BLEND_GREEN),
            RegisterFloat(registers, RB_BLEND_BLUE), RegisterFloat(registers, RB_BLEND_ALPHA) };
        if (!known || memcmp(state.blendFactor, blendFactor, sizeof(blendFactor)) != 0)
        {
            g.list->OMSetBlendFactor(blendFactor);
            memcpy(state.blendFactor, blendFactor, sizeof(blendFactor));
        }
        if (!known || state.stencilRef != (stencilRefMask & 0xFF))
        {
            g.list->OMSetStencilRef(stencilRefMask & 0xFF);
            state.stencilRef = stencilRefMask & 0xFF;
        }
        state.valid = true;

        // Experiment (AR_FORCE_SHADOW_CLEAR): the whole shadow map depth buffer cleared at the first clear of the far
        // cascade (the strip 960,0-1024,1024 at EDRAM base 128, pitch 1040).
        static const bool forceShadowClear = AR_ENV("AR_FORCE_SHADOW_CLEAR") != nullptr;
        if (forceShadowClear && depthTarget && draw.initiator.PrimType() == PrimitiveType::RectangleList &&
            DepthInfo{ registers[RB_DEPTH_INFO] }.Base() == 128 && SurfaceInfo{ registers[RB_SURFACE_INFO] }.Pitch() == 1040 &&
            SurfaceInfo{ registers[RB_SURFACE_INFO] }.MsaaSamples() == 0)
        {
            g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
        }

        bool queried = false;
        if (!g.occlusionLabel.empty())
        {
            auto& labels = g.occlusionLabels[g.frameIndex];
            if (labels.size() < State::PROBE_OFFSET)
            {
                g.list->BeginQuery(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                    g.frameIndex * State::OCCLUSIONS_PER_FRAME + uint32_t(labels.size()));
                if (g.statsHeap)
                    g.list->BeginQuery(g.statsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                        g.frameIndex * State::OCCLUSIONS_PER_FRAME + uint32_t(labels.size()));
                // What the GPU reads for the draw: indices, vertex constants (the first 64), system constants, and the
                // first vertex buffer when it is in the vertex cache (which mid-frame flushes don't reuse).
                auto& inputs = g.occlusionInputs[g.frameIndex];
                static const bool gpuCheck = g.gpuCheckReadback != nullptr;
                static const bool detailed = gpuCheck || (atoi(AR_ENV("AR_SHADOW_PIXELS")) >= 2 && atoi(AR_ENV("AR_SHADOW_PIXELS")) <= 4);
                uint64_t vertexOffset = 0, vertexSize = 0;
                if (detailed && !vertexRanges.empty())
                {
                    auto [index, size] = vertexRanges[0];
                    uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
                    vertexOffset = uint32_t(address + uint32_t(system.vertexDataOffsets[index]));
                    vertexSize = vertexOffset >= VERTEX_CACHE_BASE || gpuCheck ? std::min<uint32_t>(size, 65536) : 0;
                }
                uint64_t ranges[4][2] = { { indexed ? indexView.BufferLocation - g.uploadGpu : 0, indexed ? indexView.SizeInBytes : 0 },
                    { vertexConstants - g.uploadGpu, 32 * 16 }, { systemConstants - g.uploadGpu, sizeof(system) }, { vertexOffset, vertexSize } };
                for (auto& [offset, size] : ranges)
                {
                    if (!detailed)
                        size = 0;
                    uint64_t hash = size ? HashMemory(g.uploadCpu + offset, size) : 0;
                    inputs.push_back({ offset, size, hash });
                    g.occlusionLabel += fmt::format(" {:04X}", uint32_t(hash & 0xFFFF));
                }
                g.occlusionLabel += fmt::format(" p {:X} t {} n {} d {:X} {:X} {}x{} sc {},{}-{},{} vp {}x{}", uintptr_t(pipeline) & 0xFFFFFF,
                    uint32_t(topology), count, dsv.ptr & 0xFFFFFF, uintptr_t(depthTarget ? depthTarget->resource.Get() : nullptr) & 0xFFFFFF,
                    depthTarget ? depthTarget->width : 0, depthTarget ? depthTarget->height : 0, scissor.left, scissor.top, scissor.right,
                    scissor.bottom, viewportWidth, viewportHeight);
                // Whether the depth target's descriptor still holds what was written when it was created.
                if (depthTarget)
                {
                    auto found = g_dsvSnapshots.find(depthTarget->dsv);
                    bool same = found != g_dsvSnapshots.end() && memcmp(found->second.data(),
                        reinterpret_cast<const void*>(CpuDsv(depthTarget->dsv).ptr), found->second.size()) == 0;
                    g.occlusionLabel += same ? " dsv-ok" : " DSV-CHANGED";
                }
                // Where the constants and indices are (upload buffer offsets).
                g.occlusionLabel += fmt::format(" up {:X} {:X} {:X}", vertexConstants - g.uploadGpu, systemConstants - g.uploadGpu,
                    indexed ? indexView.BufferLocation - g.uploadGpu : 0);
                labels.push_back(std::move(g.occlusionLabel));
                queried = true;
            }
            g.occlusionLabel.clear();
        }
        if (indexed)
        {
            g.list->IASetIndexBuffer(&indexView);
            g.list->DrawIndexedInstanced(count, 1, 0, 0, 0);
        }
        else
        {
            g.list->DrawInstanced(count, 1, 0, 0);
        }
        if (queried)
        {
            uint32_t query = g.frameIndex * State::OCCLUSIONS_PER_FRAME + uint32_t(g.occlusionLabels[g.frameIndex].size()) - 1;
            g.list->EndQuery(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
            if (g.statsHeap)
                g.list->EndQuery(g.statsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, query);

            // AR_SHADOW_GPUCHECK: what the GPU sees of the draw's inputs, copied for comparison.
            if (g.gpuCheckReadback)
            {
                auto& inputs = g.occlusionInputs[g.frameIndex];
                auto& offsets = g.gpuCheckOffsets[g.frameIndex];
                uint64_t& used = g.gpuCheckUsed[g.frameIndex];
                for (size_t j = inputs.size() - 4; j < inputs.size(); j++)
                {
                    uint64_t size = inputs[j][1];
                    if (size == 0 || used + size > State::GPU_CHECK_SIZE)
                    {
                        offsets.push_back(UINT64_MAX);
                        continue;
                    }
                    uint64_t destination = uint64_t(g.frameIndex) * State::GPU_CHECK_SIZE + used;
                    g.list->CopyBufferRegion(g.gpuCheckReadback.Get(), destination, g.uploadBuffer.Get(), inputs[j][0], size);
                    offsets.push_back(destination);
                    used = (used + size + 255) & ~255ull;
                }
            }

            // The same draw again without depth testing or writes, for the area it rasterizes.
            static const bool probe = atoi(AR_ENV("AR_SHADOW_PIXELS")) >= 3;
            PipelineKey probeKey = key;
            probeKey.depthControl = 0;
            // AR_SHADOW_PIXELS=6: as points (vertices that are finite still count when every triangle collapsed).
            static const bool points = atoi(AR_ENV("AR_SHADOW_PIXELS")) >= 6;
            if (points)
            {
                probeKey.topologyType = uint8_t(D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
                probeKey.stripCut = 0;
            }
            ID3D12PipelineState* probePipeline = probe ? GetPipeline(probeKey, vertexShader, pixelShader) : nullptr;
            // AR_SHADOW_PIXELS=5: with the viewport transform shrunk 64 times, so what lands within 64 screens of the
            // target still counts (geometry moved away versus geometry gone).
            static const bool shrink = atoi(AR_ENV("AR_SHADOW_PIXELS")) >= 5;
            D3D12_GPU_VIRTUAL_ADDRESS shrunk = 0;
            // AR_SHADOW_FRESH=<letters>: the probe reads fresh copies of the vertex constants (c, from the registers),
            // the vertex data (v, from guest memory) or the indices (i), to find which one the GPU reads wrongly.
            static const std::string fresh = AR_ENV("AR_SHADOW_FRESH") ? AR_ENV("AR_SHADOW_FRESH") : "";
            D3D12_GPU_VIRTUAL_ADDRESS freshConstants = 0;
            D3D12_INDEX_BUFFER_VIEW freshIndices{};
            if (probePipeline && shrink)
            {
                SystemConstants shrunkSystem = system;
                // Shrunk about the guest viewport's centre, which stays where it was.
                for (int c = 0; c < 2; c++)
                    shrunkSystem.ndcScale[c] /= 64.0f;
                if (fresh.find('v') != std::string::npos)
                {
                    for (auto [index, size] : vertexRanges)
                    {
                        uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
                        auto copy = Upload(size, 16);
                        memcpy(copy.cpu, g_memory.Translate<uint8_t>(PhysicalToVirtual(address)), size);
                        shrunkSystem.vertexDataOffsets[index] = int32_t(copy.offset) - int32_t(address);
                    }
                }
                shrunk = UploadConstants(shrunkSystem);
                if (fresh.find('c') != std::string::npos)
                {
                    uint32_t base = std::min<uint32_t>(registers[0x2307] & 0x1FF, 192);
                    auto copy = Upload(64 * 16, 256);
                    auto* out = reinterpret_cast<uint32_t*>(copy.cpu);
                    const auto* in = reinterpret_cast<const uint32_t*>(&registers[SHADER_CONSTANT_000_X + base * 4]);
                    for (uint32_t i = 0; i < 64 * 4; i++)
                        out[i] = __builtin_bswap32(in[i]);
                    freshConstants = copy.gpu;
                }
                if (fresh.find('i') != std::string::npos && indexed)
                {
                    auto copy = Upload((indexView.SizeInBytes + 3) & ~3u, 16);
                    std::vector<uint8_t> bytes(indexView.SizeInBytes);
                    memcpy(bytes.data(), g.uploadCpu + (indexView.BufferLocation - g.uploadGpu), bytes.size());
                    memcpy(copy.cpu, bytes.data(), bytes.size());
                    freshIndices = { copy.gpu, indexView.SizeInBytes, indexView.Format };
                }
            }
            // AR_SHADOW_REBIND: everything the draw uses bound again from scratch before the probe.
            static const bool rebind = AR_ENV("AR_SHADOW_REBIND") != nullptr;
            if (probePipeline && rebind)
            {
                BindHeaps();
                g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, systemConstants);
                g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, vertexConstants);
                g.list->SetGraphicsRootConstantBufferView(ROOT_PIXEL_FLOAT_CONSTANTS, pixelConstants);
                g.list->SetGraphicsRootShaderResourceView(ROOT_VERTEX_DATA, g.uploadGpu);
                g.list->OMSetRenderTargets(key.renderTargetCount, rtvs, FALSE, depthTarget ? &dsv : nullptr);
                D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(viewportWidth), float(viewportHeight), 0.0f, 1.0f };
                g.list->RSSetViewports(1, &viewport);
                g.list->RSSetScissorRects(1, &scissor);
                g.list->IASetPrimitiveTopology(topology);
                if (indexed)
                    g.list->IASetIndexBuffer(&indexView);
            }
            static const bool late = AR_ENV("AR_SHADOW_LATE") != nullptr;
            if (late && probePipeline && g.shadowResolves == 0)
            {
                g.lateProbes.push_back({ probePipeline, systemConstants, vertexConstants, pixelConstants, indexView, indexed, count, topology,
                    query + State::PROBE_OFFSET, scissor, viewportWidth, viewportHeight });
                probePipeline = nullptr;
            }
            else if (late)
            {
                probePipeline = nullptr;
            }
            bool fromDefault = probePipeline && g.defaultCopy;
            if (fromDefault)
            {
                auto copy = [&](uint64_t offset, uint64_t size)
                    {
                        if (size)
                            g.list->CopyBufferRegion(g.defaultCopy.Get(), offset, g.uploadBuffer.Get(), offset, size);
                    };
                copy(systemConstants - g.uploadGpu, 2560);
                copy(vertexConstants - g.uploadGpu, 4096);
                copy(pixelConstants - g.uploadGpu, 256);
                if (indexed)
                    copy(indexView.BufferLocation - g.uploadGpu, (indexView.SizeInBytes + 3) & ~3u);
                for (auto [index, size] : vertexRanges)
                {
                    uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
                    copy(uint32_t(address + uint32_t(system.vertexDataOffsets[index])), (size + 15) & ~15u);
                }
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = g.defaultCopy.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                g.list->ResourceBarrier(1, &barrier);
                // AR_SHADOW_DEFAULT_COPY=2: the copies and barrier only, still reading upload memory.
                static const bool copyOnly = atoi(AR_ENV("AR_SHADOW_DEFAULT_COPY")) == 2;
                D3D12_GPU_VIRTUAL_ADDRESS defaultGpu = copyOnly ? g.uploadGpu : g.defaultCopy->GetGPUVirtualAddress();
                // AR_SHADOW_DEFAULT_ONLY=<letters>: only these from video memory: c constants, v vertex data, i indices.
                static const std::string only = AR_ENV("AR_SHADOW_DEFAULT_ONLY") ? AR_ENV("AR_SHADOW_DEFAULT_ONLY") : "cvi";
                auto pick = [&](char what) { return only.find(what) != std::string::npos ? defaultGpu : g.uploadGpu; };
                g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, pick('c') + (systemConstants - g.uploadGpu));
                g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, pick('c') + (vertexConstants - g.uploadGpu));
                g.list->SetGraphicsRootConstantBufferView(ROOT_PIXEL_FLOAT_CONSTANTS, pick('c') + (pixelConstants - g.uploadGpu));
                g.list->SetGraphicsRootShaderResourceView(ROOT_VERTEX_DATA, pick('v'));
                if (indexed)
                {
                    D3D12_INDEX_BUFFER_VIEW view = indexView;
                    view.BufferLocation = pick('i') + (indexView.BufferLocation - g.uploadGpu);
                    g.list->IASetIndexBuffer(&view);
                }
            }
            if (!late)
                g.list->BeginQuery(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query + State::PROBE_OFFSET);
            if (probePipeline)
            {
                g.list->SetPipelineState(probePipeline);
                if (shrunk)
                    g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, shrunk);
                if (freshConstants)
                    g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, freshConstants);
                if (freshIndices.BufferLocation)
                    g.list->IASetIndexBuffer(&freshIndices);
                if (points)
                    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
                if (indexed)
                    g.list->DrawIndexedInstanced(count, 1, 0, 0, 0);
                else
                    g.list->DrawInstanced(count, 1, 0, 0);
                if (points)
                    g.list->IASetPrimitiveTopology(topology);
                g.list->SetPipelineState(pipeline);
                if (shrunk)
                    g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, systemConstants);
                if (freshConstants)
                    g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, vertexConstants);
                if (freshIndices.BufferLocation)
                    g.list->IASetIndexBuffer(&indexView);
            }
            if (!late)
                g.list->EndQuery(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query + State::PROBE_OFFSET);
            if (fromDefault)
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = g.defaultCopy.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g.list->ResourceBarrier(1, &barrier);
                g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, systemConstants);
                g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, vertexConstants);
                g.list->SetGraphicsRootConstantBufferView(ROOT_PIXEL_FLOAT_CONSTANTS, pixelConstants);
                g.list->SetGraphicsRootShaderResourceView(ROOT_VERTEX_DATA, g.uploadGpu);
                if (indexed)
                    g.list->IASetIndexBuffer(&indexView);
            }
        }

        // AR_SHADOW_SO: the draw again with its positions streamed out (depth test and writes off).
        if (!g.soLabel.empty() && g.soLabels[g.frameIndex].size() < State::SO_SLOTS)
        {
            PipelineKey soKey = key;
            soKey.depthControl = 0;
            if (ID3D12PipelineState* soPipeline = GetPipeline(soKey, vertexShader, pixelShader, true))
            {
                uint64_t slot = uint64_t(g.frameIndex) * State::SO_SLOTS + g.soLabels[g.frameIndex].size();
                uint64_t dataOffset = slot * State::SO_SLOT_SIZE, filledOffset = State::SO_FILLED_BASE + slot * 16;
                auto zero = Upload(16, 16);
                memset(zero.cpu, 0, 16);
                g.list->CopyBufferRegion(g.soBuffer.Get(), filledOffset, g.uploadBuffer.Get(), zero.offset, 16);
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = g.soBuffer.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_STREAM_OUT;
                g.list->ResourceBarrier(1, &barrier);
                D3D12_STREAM_OUTPUT_BUFFER_VIEW view{ g.soBuffer->GetGPUVirtualAddress() + dataOffset, State::SO_SLOT_SIZE,
                    g.soBuffer->GetGPUVirtualAddress() + filledOffset };
                g.list->SOSetTargets(&view);
                g.list->SetPipelineState(soPipeline);
                if (indexed)
                    g.list->DrawIndexedInstanced(count, 1, 0, 0, 0);
                else
                    g.list->DrawInstanced(count, 1, 0, 0);
                g.list->SetPipelineState(pipeline);
                g.list->SOSetTargets(nullptr);
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                g.list->ResourceBarrier(1, &barrier);
                g.list->CopyBufferRegion(g.soReadback.Get(), dataOffset, g.soBuffer.Get(), dataOffset, State::SO_SLOT_SIZE);
                g.list->CopyBufferRegion(g.soReadback.Get(), filledOffset, g.soBuffer.Get(), filledOffset, 16);
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g.list->ResourceBarrier(1, &barrier);
                g.soLabels[g.frameIndex].push_back(std::move(g.soLabel));
            }
        }
        lap.Lap(g_profile.record);
    }

    // ---- Resolves ----

    static void Resolve(const be<uint32_t>* registers)
    {
        GpuScope scope{ 2 };
        g.textureLookups.clear();
        CopyControl control{ registers[RB_COPY_CONTROL] };
        SampleScale sampleScale = GetSampleScale(registers);
        uint32_t pitch = SurfaceInfo{ registers[RB_SURFACE_INFO] }.Pitch();
        if (pitch == 0)
            return;

        // The rectangle comes from the copy draw's three vertices, in vertex fetch constant 0.
        uint32_t fetch0 = registers[SHADER_CONSTANT_FETCH_00_0], fetch1 = registers[SHADER_CONSTANT_FETCH_00_0 + 1];
        const uint8_t* vertexData = g_memory.Translate<uint8_t>(PhysicalToVirtual(fetch0 & 0x1FFFFFFC));
        float halfPixel = (registers[0x2302] & 1) == 0 ? 0.5f : 0.0f;
        float vertices[6];
        for (uint32_t i = 0; i < 6; i++)
        {
            uint32_t raw;
            memcpy(&raw, vertexData + i * 4, 4);
            raw = GpuSwap(raw, fetch1 & 3);
            memcpy(&vertices[i], &raw, 4);
        }

        auto round = [&](float value) { return int32_t(std::floor(value + halfPixel + 0.5f)); };
        int32_t x0 = round(std::min({ vertices[0], vertices[2], vertices[4] }));
        int32_t y0 = round(std::min({ vertices[1], vertices[3], vertices[5] }));
        int32_t x1 = round(std::max({ vertices[0], vertices[2], vertices[4] }));
        int32_t y1 = round(std::max({ vertices[1], vertices[3], vertices[5] }));
        if ((registers[PA_SU_SC_MODE_CNTL] >> 16) & 1)
        {
            int32_t windowX, windowY;
            GetWindowOffset(registers, windowX, windowY);
            x0 += windowX;
            x1 += windowX;
            y0 += windowY;
            y1 += windowY;
        }

        D3D12_RECT scissor = GetScissor(registers);
        x0 = std::clamp<int32_t>(x0, scissor.left, scissor.right) & ~7;
        y0 = std::clamp<int32_t>(y0, scissor.top, scissor.bottom) & ~7;
        x1 = std::min<int32_t>((std::clamp<int32_t>(x1, scissor.left, scissor.right) + 7) & ~7, int32_t(pitch));
        y1 = std::min<int32_t>((std::clamp<int32_t>(y1, scissor.top, scissor.bottom) + 7) & ~7, int32_t(RENDER_TARGET_HEIGHT / sampleScale.y));
        if (x0 >= x1 || y0 >= y1)
            return;

        uint32_t sourceSelect = control.SourceSelect();
        bool depthSource = sourceSelect >= 4;
        Texture* source = nullptr;
        if (depthSource)
        {
            DepthInfo info{ registers[RB_DEPTH_INFO] };
            source = GetRenderTarget(info.Base(), pitch * sampleScale.x, info.Format(), true);
        }
        else
        {
            ColorInfo info{ registers[sourceSelect == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + sourceSelect - 1] };
            source = GetRenderTarget(info.Base(), pitch * sampleScale.x, info.Format(), false);
        }
        if (!source)
            return;
        ConvertLayout(*source, SurfaceInfo{ registers[RB_SURFACE_INFO] }.MsaaSamples());

        D3D12_RECT rect{ x0, y0, x1, y1 };
        uint32_t destBase = registers[RB_COPY_DEST_BASE] & 0x1FFFFFFF;

        // Diagnostics (AR_SHADOW_LOG): resolves out of the shadow map, between its draws (S lines).
        static const bool shadowLog = AR_ENV("AR_SHADOW_LOG") != nullptr;
        if (shadowLog && pitch == 1040)
        {
            static const int64_t first = AR_ENV("AR_DUMP_FRAME") ? atoll(AR_ENV("AR_DUMP_FRAME")) : 0;
            static const int64_t frames = AR_ENV("AR_DUMP_FRAMES") ? atoll(AR_ENV("AR_DUMP_FRAMES")) : 1;
            if (int64_t(g.frameNumber) >= first && int64_t(g.frameNumber) < first + frames)
            {
                LOGF_INFO("SR {} {:08X} {},{}-{},{} source {} marker {:08X}", g.frameNumber, destBase, x0, y0, x1, y1,
                    sourceSelect, CurrentDrawMarker());
                if (destBase != 0x10C22000)
                    g.shadowResolves++;

                // AR_SHADOW_LATE: the far cascade's probes, now.
                if (destBase == 0x0F801000 && depthSource && !g.lateProbes.empty())
                {
                    Transition(*source, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    auto dsv = CpuDsv(source->dsv);
                    g.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                    for (const auto& probe : g.lateProbes)
                    {
                        SetViewportAndScissor(probe.viewportWidth, probe.viewportHeight, probe.scissor);
                        g.list->SetGraphicsRootShaderResourceView(ROOT_VERTEX_DATA, g.uploadGpu);
                        g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, probe.systemConstants);
                        g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, probe.vertexConstants);
                        g.list->SetGraphicsRootConstantBufferView(ROOT_PIXEL_FLOAT_CONSTANTS, probe.pixelConstants);
                        g.list->SetPipelineState(probe.pipeline);
                        g.list->IASetPrimitiveTopology(probe.topology);
                        g.list->BeginQuery(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, probe.query);
                        if (probe.indexed)
                        {
                            g.list->IASetIndexBuffer(&probe.indexView);
                            g.list->DrawIndexedInstanced(probe.count, 1, 0, 0, 0);
                        }
                        else
                        {
                            g.list->DrawInstanced(probe.count, 1, 0, 0);
                        }
                        g.list->EndQuery(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, probe.query);
                    }
                    g.drawState.valid = false;
                }
                g.lateProbes.clear();
            }
        }
        if (AR_ENV("AR_LOG_PRESENT") && g.frameNumber >= 300 && g.frameNumber < 303 && destBase == 0x0CEF0000)
            LOGF_INFO("frame {}: resolve to front buffer {},{}-{},{} from {} (base {}, pitch {}), command {}, clear {:X}", g.frameNumber,
                x0, y0, x1, y1, sourceSelect, ColorInfo{ registers[RB_COLOR_INFO] }.Base(), pitch, control.Command(), (control.value >> 8) & 3);
        if (TraceThisFrame())
            LOGF_INFO("resolve window offset {:08X} mode {:X} scissor {:08X} {:08X} dest pitch {:08X} info {:08X}",
                uint32_t(registers[PA_SC_WINDOW_OFFSET]), uint32_t(registers[PA_SU_SC_MODE_CNTL]), uint32_t(registers[PA_SC_WINDOW_SCISSOR_TL]),
                uint32_t(registers[PA_SC_WINDOW_SCISSOR_BR]), uint32_t(registers[RB_COPY_DEST_PITCH]), uint32_t(registers[RB_COPY_DEST_INFO]));
        if (TraceThisFrame())
            LOGF_INFO("resolve {},{}-{},{} from {} (pitch {}) to {:08X} format {} command {} clear {:X} vertices {} {} {} {} {} {}",
                x0, y0, x1, y1, sourceSelect, pitch, destBase, CopyDestInfo{ registers[RB_COPY_DEST_INFO] }.Format(), control.Command(),
                (control.value >> 8) & 3, vertices[0], vertices[1], vertices[2], vertices[3], vertices[4], vertices[5]);
        CopyDestInfo destInfo{ registers[RB_COPY_DEST_INFO] };
        CopyDestPitch destPitch{ registers[RB_COPY_DEST_PITCH] };
        if (AR_ENV("AR_LOG_RESOLVES"))
        {
            static std::set<uint64_t> logged;
            if (logged.insert((uint64_t(destBase) << 32) | (uint32_t(x1 - x0) << 16) | uint32_t(y1 - y0)).second)
                LOGF_INFO("frame {}: resolve {}x{} from {} to 0x{:08X} format {} pitch {}x{}", g.frameNumber, x1 - x0, y1 - y0,
                    sourceSelect, destBase, destInfo.Format(), destPitch.Pitch(), destPitch.Height());
        }

        if (destBase != 0 && control.Command() != 3)
        {
            DXGI_FORMAT destFormat = GetHostTextureFormat(destInfo.Format());
            if (depthSource && (destInfo.Format() == 22 || destInfo.Format() == 23)) // 24_8, 24_8_FLOAT: raw words.
                destFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

            if (depthSource && destFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                if (g.warnedFormats.insert(0x200 | destInfo.Format()).second)
                    LOGF_WARNING("depth resolves to format {} are not supported", destInfo.Format());
            }
            else if (destFormat == DXGI_FORMAT_UNKNOWN || (destFormat >= DXGI_FORMAT_BC1_TYPELESS && destFormat <= DXGI_FORMAT_BC5_SNORM))
            {
                if (g.warnedFormats.insert(0x100 | destInfo.Format()).second)
                    LOGF_WARNING("resolves to format {} are not supported", destInfo.Format());
            }
            else
            {
                uint32_t destWidth = std::max<uint32_t>(destPitch.Pitch(), uint32_t(x1));
                uint32_t destHeight = std::max<uint32_t>(destPitch.Height(), uint32_t(y1));
                uint32_t rowBytes = destWidth * BytesPerPixel(destFormat);

                // Resolves can write into part of a texture resolved before (games render in strips and point each
                // strip's resolve at its rows), so they go to the resolved texture holding the address when it fits.
                uint32_t row = 0;
                ResolvedTexture* target = FindResolved(destBase, rowBytes, row);
                if (target && (target->texture.format != destFormat || target->texture.width != destWidth * g_scale ||
                    row + uint32_t(y1) > target->texture.height / g_scale || (row == 0 && target->texture.height != destHeight * g_scale)))
                {
                    target = nullptr;
                }

                if (!target)
                {
                    ResolvedTexture created;
                    created.texture = CreateTexture(destWidth * g_scale, destHeight * g_scale, 1, false, destFormat,
                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    created.format = destInfo.Format();
                    created.rowBytes = rowBytes;
                    row = 0;
                    if (created.texture.resource)
                    {
                        const float black[4] = {};
                        g.list->ClearRenderTargetView(CpuRtv(created.texture.rtv), black, 0, nullptr);

                        // Resolved textures inside the new one's memory become part of it.
                        uint64_t end = uint64_t(destBase) + uint64_t(rowBytes) * destHeight;
                        for (auto it = g.resolvedTextures.begin(); it != g.resolvedTextures.end();)
                        {
                            auto& inner = it->second;
                            bool inside = it->first >= destBase && it->first < end && inner.rowBytes == rowBytes &&
                                (it->first - destBase) % rowBytes == 0;
                            uint32_t innerRow = inside ? (it->first - destBase) / rowBytes : 0;
                            if (inside && inner.texture.format == destFormat && inner.texture.width == destWidth * g_scale &&
                                innerRow + inner.texture.height / g_scale <= destHeight)
                            {
                                Transition(inner.texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
                                Transition(created.texture, D3D12_RESOURCE_STATE_COPY_DEST);
                                D3D12_TEXTURE_COPY_LOCATION source{ inner.texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                D3D12_TEXTURE_COPY_LOCATION dest{ created.texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                g.list->CopyTextureRegion(&dest, 0, innerRow * g_scale, 0, &source, nullptr);
                            }

                            if (inside || it->first == destBase)
                            {
                                ReleaseTexture(inner.texture);
                                it = g.resolvedTextures.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                        }

                        target = &g.resolvedTextures.insert_or_assign(destBase, std::move(created)).first->second;
                    }
                }

                if (target)
                {
                    auto& resolved = *target;
                    resolved.rawDepth = depthSource;
                    resolved.version = ++g.resolveVersion;
                    struct BlitConstants
                    {
                        int32_t sourceOffset[2];
                        uint32_t sourceIndex;
                        uint32_t swapRedBlue;
                        float scale[4];
                        int32_t sampleScale[2];
                        uint32_t resolutionScale;
                    } constants{ { 0, -int32_t(row) }, source->srv, destInfo.Swap() ? 1u : 0u };
                    constants.resolutionScale = g_scale;
                    constants.sampleScale[0] = int32_t(sampleScale.x);
                    constants.sampleScale[1] = int32_t(sampleScale.y);
                    float scale = std::ldexp(1.0f, destInfo.ExpBias());
                    for (float& value : constants.scale)
                        value = scale;

                    struct DepthBlitConstants
                    {
                        int32_t sourceOffset[2];
                        uint32_t depthIndex;
                        uint32_t stencilIndex;
                        uint32_t floatDepth;
                        int32_t sampleScale[2];
                        uint32_t swapHalves;
                        uint32_t resolutionScale;
                    } depthConstants{ { 0, -int32_t(row) }, source->srv, source->stencilSrv,
                        DepthInfo{ registers[RB_DEPTH_INFO] }.Format() == 1 ? 1u : 0u, { int32_t(sampleScale.x), int32_t(sampleScale.y) },
                        0u, g_scale };

                    Transition(*source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    Transition(resolved.texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    auto rtv = CpuRtv(resolved.texture.rtv);
                    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    D3D12_RECT destRect{ rect.left, rect.top + LONG(row), rect.right, rect.bottom + LONG(row) };
                    SetViewportAndScissor(resolved.texture.width, resolved.texture.height, Scaled(destRect));
                    g.list->SetPipelineState(GetBlitPipeline(destFormat, false, depthSource));
                    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS,
                        depthSource ? UploadConstants(depthConstants) : UploadConstants(constants));
                    g.list->DrawInstanced(3, 1, 0, 0);

                    if (TraceThisFrame() && source->format == DXGI_FORMAT_R8G8B8A8_UNORM && AR_ENV("AR_DUMP_SOURCES"))
                    {
                        static int sourceIndex;
                        SaveScreenshot(*source, source->width, 1024, fmt::format("screenshots/source_{:02}_{:08X}.bmp", sourceIndex++, destBase));
                    }
                    // AR_DUMP_ONLY limits the dumps to resolves to the listed addresses (hex, comma separated).
                    static const bool shadowReadback = AR_ENV("AR_SHADOW_READBACK") != nullptr;
                    if (shadowReadback && destBase == 0x0F801000)
                    {
                        Texture& texture = resolved.texture;
                        D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
                        UINT64 totalSize;
                        g.device->GetCopyableFootprints(&desc, 0, 1, 0, &g.shadowFootprint[g.frameIndex], nullptr, nullptr, &totalSize);
                        auto& readback = g.shadowReadback[g.frameIndex];
                        if (!readback || readback->GetDesc().Width < totalSize)
                        {
                            readback.Reset();
                            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
                            D3D12_RESOURCE_DESC bufferDesc{};
                            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            bufferDesc.Width = totalSize;
                            bufferDesc.Height = 1;
                            bufferDesc.DepthOrArraySize = 1;
                            bufferDesc.MipLevels = 1;
                            bufferDesc.SampleDesc.Count = 1;
                            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                IID_PPV_ARGS(&readback));
                        }
                        if (readback)
                        {
                            Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            D3D12_TEXTURE_COPY_LOCATION source{ texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                            D3D12_TEXTURE_COPY_LOCATION dest{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                            dest.PlacedFootprint = g.shadowFootprint[g.frameIndex];
                            g.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
                            g.shadowCopied[g.frameIndex] = true;
                            g.shadowCopyFrame[g.frameIndex] = g.frameNumber;
                            g.shadowCopyFormat = texture.format;
                        }
                    }
                    static const std::vector<uint32_t> dumpOnly = []
                        {
                            std::vector<uint32_t> addresses;
                            for (const char* text = AR_ENV("AR_DUMP_ONLY"); text && *text;)
                            {
                                char* end;
                                addresses.push_back(uint32_t(strtoul(text, &end, 16)));
                                text = *end == ',' ? end + 1 : end;
                                if (end == text)
                                    break;
                            }
                            return addresses;
                        }();
                    // AR_DUMP_FRAME / AR_DUMP_FRAMES dump resolves like a traced frame without logging every draw.
                    static const int64_t dumpFrame = AR_ENV("AR_DUMP_FRAME") ? atoll(AR_ENV("AR_DUMP_FRAME")) : -1;
                    static const int64_t dumpFrames = AR_ENV("AR_DUMP_FRAMES") ? atoll(AR_ENV("AR_DUMP_FRAMES")) : 1;
                    bool dumpThisFrame = TraceThisFrame() ||
                        (dumpFrame >= 0 && int64_t(g.frameNumber) >= dumpFrame && int64_t(g.frameNumber) < dumpFrame + dumpFrames);
                    if (dumpThisFrame && (destFormat == DXGI_FORMAT_R8G8B8A8_UNORM || destFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || destFormat == DXGI_FORMAT_R32_FLOAT) &&
                        (dumpOnly.empty() || std::find(dumpOnly.begin(), dumpOnly.end(), destBase) != dumpOnly.end()))
                    {
                        static int resolveIndex;
                        SaveScreenshot(resolved.texture, destWidth * g_scale, destHeight * g_scale, fmt::format("screenshots/resolve_{:06}_{:02}_{:08X}.bmp", g.frameNumber, resolveIndex++ % 100, destBase));
                    }
                }
            }
        }

        // Clears of the source after the copy.
        if (control.ColorClear() && !depthSource)
        {
            uint32_t clear = registers[RB_COLOR_CLEAR];
            float color[4] = { float(clear & 0xFF) / 255.0f, float((clear >> 8) & 0xFF) / 255.0f,
                float((clear >> 16) & 0xFF) / 255.0f, float(clear >> 24) / 255.0f };
            Transition(*source, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_RECT sourceRect = ScaleRect(rect, sampleScale);
            D3D12_RECT hostRect = Scaled(sourceRect);
            g.list->ClearRenderTargetView(CpuRtv(source->rtv), color, 1, &hostRect);
            ColorInfo info{ registers[sourceSelect == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + sourceSelect - 1] };
            RecordWrite({ info.Base(), pitch * sampleScale.x, info.Format(), false }, sourceRect);
        }

        if (control.DepthClear())
        {
            DepthInfo info{ registers[RB_DEPTH_INFO] };
            if (Texture* depth = GetRenderTarget(info.Base(), pitch * sampleScale.x, info.Format(), true))
            {
                uint32_t clear = registers[RB_DEPTH_CLEAR];
                D3D12_RECT depthRect = ScaleRect(rect, sampleScale);
                D3D12_RECT hostRect = Scaled(depthRect);
                Transition(*depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                g.list->ClearDepthStencilView(CpuDsv(depth->dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    float(clear >> 8) / 16777215.0f, UINT8(clear & 0xFF), 1, &hostRect);
                RecordWrite({ info.Base(), pitch * sampleScale.x, info.Format(), true }, depthRect);
            }
        }
    }

    // ---- Presentation ----

    static bool LoadControllerDiagram()
    {
        static bool tried;
        if (g_diagramTexture.resource)
            return true;
        if (tried)
            return false;
        std::ifstream file(CONTROLLER_DIAGRAM_FILE, std::ios::binary);
        BITMAPFILEHEADER header{};
        BITMAPINFOHEADER info{};
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || !file.read(reinterpret_cast<char*>(&info), sizeof(info)) ||
            info.biBitCount != 32 || info.biWidth <= 0 || info.biHeight <= 0)
            return false;
        tried = true;
        DecodedTexture decoded{};
        decoded.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        decoded.width = uint32_t(info.biWidth);
        decoded.height = uint32_t(info.biHeight);
        decoded.depth = 1;
        decoded.rowPitch = decoded.width * 4;
        decoded.rowCount = decoded.height;
        decoded.data.resize(size_t(decoded.rowPitch) * decoded.height);
        std::vector<uint8_t> row(decoded.rowPitch);
        for (uint32_t y = 0; y < decoded.height; y++)
        {
            if (!file.read(reinterpret_cast<char*>(row.data()), row.size()))
                return false;
            uint8_t* out = &decoded.data[size_t(decoded.height - 1 - y) * decoded.rowPitch];
            for (uint32_t x = 0; x < decoded.width; x++)
            {
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = 255;
            }
        }
        g_diagramTexture = CreateTexture(decoded.width, decoded.height, 1, false, decoded.format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_diagramTexture.resource)
            return false;
        UploadTexture(g_diagramTexture, decoded);
        return true;
    }

    static void DrawOverlay(float originX, float originY, float scale)
    {
        std::vector<OverlayQuad> quads;
        kbm::GetOverlay(quads);
        if (quads.empty())
            return;
        if (!g.overlayPipeline)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g.rootSignature.Get();
            desc.VS = Bytecode(g.blitVertexShader);
            desc.PS = Bytecode(g.overlayPixelShader);
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g.overlayPipeline))))
                return;
        }

        struct OverlayConstants
        {
            float origin[2], size[2], uvOrigin[2], uvSize[2];
            float tint[4];
            uint32_t sourceIndex, samplerIndex, picture;
        };
        const Texture& atlas = GetKeyboardArt(3);
        const Texture& keyboardPage = GetKeyboardArt(4);
        const Texture& controllerPage = GetKeyboardArt(5);
        g.list->SetPipelineState(g.overlayPipeline.Get());
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const OverlayQuad& quad : quads)
        {
            OverlayConstants constants{};
            const Texture* texture = &atlas;
            float x0 = 0.0f, y0 = 0.0f, width = 4.0f / 1024.0f, height = 4.0f / 256.0f;
            if (quad.item >= 0 && size_t(quad.item) < std::size(g_overlayRects))
            {
                const auto* rect = g_overlayRects[quad.item];
                x0 = float(rect[0]) / 1024.0f;
                y0 = float(rect[1]) / 256.0f;
                width = float(rect[2]) / 1024.0f;
                height = float(rect[3]) / 256.0f;
            }
            else if (quad.item == -2 || quad.item == -3)
            {
                texture = quad.item == -2 ? &keyboardPage : &controllerPage;
                width = height = 1.0f;
            }
            else if ((quad.item == -4 || quad.item == -5 || quad.item == -6 || quad.item == -9) && GameFontReady(quad.item == -9 ? 3 : -4 - quad.item))
            {
                texture = &g_gameFonts[quad.item == -9 ? 3 : -4 - quad.item]->texture;
                float atlasWidth = quad.item == -4 ? 256.0f : quad.item == -5 ? 2048.0f : quad.item == -9 ? 512.0f : 128.0f;
                float atlasHeight = quad.item == -4 ? 256.0f : quad.item == -5 ? 1024.0f : quad.item == -9 ? 512.0f : 64.0f;
                x0 = quad.texels[0] / atlasWidth;
                y0 = quad.texels[1] / atlasHeight;
                width = (quad.texels[2] - quad.texels[0]) / atlasWidth;
                height = (quad.texels[3] - quad.texels[1]) / atlasHeight;
            }
            else if (quad.item == -7 || quad.item == -8)
            {
                texture = &GetKeyboardArt(quad.item == -7 ? 7 : 8);
                float atlasWidth = quad.item == -7 ? 1280.0f : 1024.0f, atlasHeight = quad.item == -7 ? 720.0f : 128.0f;
                x0 = quad.texels[0] / atlasWidth;
                y0 = quad.texels[1] / atlasHeight;
                width = (quad.texels[2] - quad.texels[0]) / atlasWidth;
                height = (quad.texels[3] - quad.texels[1]) / atlasHeight;
            }
            bool picture = false;
            float quadTop = quad.y, quadHeight2 = quad.height;
            if (quad.item == -3 && LoadControllerDiagram())
            {
                // The part of the loading screen with the diagram: under its title, above its tip.
                texture = &g_diagramTexture;
                picture = true;
                y0 = 112.0f / 720.0f;
                height = (566.0f - 112.0f) / 720.0f;
                quadTop = 133.0f;
                quadHeight2 = 566.0f - 112.0f;
            }
            if (!texture->resource)
                continue;
            // Text at the size it has in the atlas, whatever was asked for.
            float quadWidth = quad.width, quadHeight = quadHeight2;
            if (quad.item >= 0 && size_t(quad.item) < std::size(g_overlayRects))
            {
                quadWidth = float(g_overlayRects[quad.item][2]);
                quadHeight = float(g_overlayRects[quad.item][3]);
            }
            float left = originX + quad.x * scale, top = originY + quadTop * scale, right = left + quadWidth * scale, bottom = top + quadHeight * scale;
            constants.origin[0] = left;
            constants.origin[1] = top;
            constants.size[0] = right - left;
            constants.size[1] = bottom - top;
            constants.uvOrigin[0] = x0;
            constants.uvOrigin[1] = y0;
            constants.uvSize[0] = width;
            constants.uvSize[1] = height;
            memcpy(constants.tint, quad.color, sizeof(constants.tint));
            constants.sourceIndex = texture->srv;
            constants.samplerIndex = GetLinearClampSampler();
            constants.picture = picture ? 1 : quad.sharpen ? 2 : 0;
            D3D12_VIEWPORT viewport{ left, top, right - left, bottom - top, 0.0f, 1.0f };
            D3D12_RECT scissor{ LONG(left), LONG(top), LONG(right + 1.0f), LONG(bottom + 1.0f) };
            g.list->RSSetViewports(1, &viewport);
            g.list->RSSetScissorRects(1, &scissor);
            g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
            g.list->DrawInstanced(3, 1, 0, 0);
        }
    }

    static std::filesystem::path g_screenshotFolder;

    void SetPipelineCacheFile(const std::filesystem::path& path)
    {
        g.pipelineCachePath = path;
    }

    // Writes the pipeline library when pipelines were added (every few seconds of play).
    static void SavePipelineCache()
    {
        if (!g.pipelineLibrary || !g.pipelineLibraryDirty)
            return;

        g.pipelineLibraryDirty = false;
        std::vector<uint8_t> data(g.pipelineLibrary->GetSerializedSize());
        if (FAILED(g.pipelineLibrary->Serialize(data.data(), data.size())))
            return;

        auto temp = g.pipelineCachePath;
        temp += ".tmp";
        {
            std::ofstream file(temp, std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        std::error_code ec;
        std::filesystem::rename(temp, g.pipelineCachePath, ec);
    }

    uint64_t GetFrameNumber()
    {
        return g.frameNumber;
    }

    static std::mutex g_screenshotRequestMutex;
    static std::string g_screenshotRequest;

    void RequestScreenshot(const std::string& name)
    {
        std::lock_guard lock(g_screenshotRequestMutex);
        g_screenshotRequest = name;
    }

    void EnableScreenshots(const std::filesystem::path& folder)
    {
        g_screenshotFolder = folder;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
    }

    // Reads a texture back (waiting for the GPU) and saves it as a 32 bit BMP, for debugging.
    void SaveScreenshot(Texture& texture, uint32_t width, uint32_t height, const std::filesystem::path& path, bool swapRedBlue)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT64 totalSize;
        D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
        g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalSize);

        ComPtr<ID3D12Resource> readback;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = totalSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&readback))))
            return;

        Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{ texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION dest{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dest.PlacedFootprint = footprint;
        g.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
        FlushAndWait();

        width = std::min(width, texture.width);
        height = std::min(height, texture.height);
        uint8_t* data;
        readback->Map(0, nullptr, reinterpret_cast<void**>(&data));
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row = data + footprint.Footprint.RowPitch * (height - 1 - y);
            for (uint32_t x = 0; x < width; x++)
            {
                uint8_t* out = &pixels[(size_t(y) * width + x) * 4];
                if (texture.format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    // Half floats, clamped to 0-1.
                    uint8_t rgb[3];
                    for (uint32_t c = 0; c < 3; c++)
                    {
                        uint16_t half;
                        memcpy(&half, row + x * 8 + c * 2, 2);
                        uint32_t exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
                        float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
                            std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
                        if (half & 0x8000)
                            value = 0.0f;
                        rgb[c] = uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    out[0] = rgb[swapRedBlue ? 0 : 2];
                    out[1] = rgb[1];
                    out[2] = rgb[swapRedBlue ? 2 : 0];
                }
                else if (texture.format == DXGI_FORMAT_R32_FLOAT)
                {
                    // Single floats: AR_DUMP_SCALE (default 100) maps to white; negative values are red.
                    static const float scale = AR_ENV("AR_DUMP_SCALE") ? float(atof(AR_ENV("AR_DUMP_SCALE"))) : 100.0f;
                    float value;
                    memcpy(&value, row + x * 4, 4);
                    uint8_t gray = uint8_t(std::clamp(std::abs(value) / scale, 0.0f, 1.0f) * 255.0f);
                    out[0] = out[1] = gray;
                    out[2] = value < 0.0f || std::isnan(value) ? 255 : gray;
                }
                else if (AR_ENV("AR_DUMP_ALPHA") && path.filename().string().rfind("resolve", 0) == 0)
                {
                    out[0] = out[1] = out[2] = row[x * 4 + 3];
                }
                else
                {
                    out[0] = row[x * 4 + (swapRedBlue ? 0 : 2)];
                    out[1] = row[x * 4 + 1];
                    out[2] = row[x * 4 + (swapRedBlue ? 2 : 0)];
                }
                out[3] = 255;
            }
        }
        readback->Unmap(0, nullptr);

        BITMAPFILEHEADER file{ 0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + pixels.size()), 0, 0,
            DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) };
        BITMAPINFOHEADER info{ sizeof(BITMAPINFOHEADER), LONG(width), LONG(height), 1, 32, BI_RGB };
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    }

    void Present(const be<uint32_t>* registers, const SwapInfo& swap)
    {
        if (!Initialize())
            return;

        static auto lastPresent = std::chrono::steady_clock::now();
        {
            // Hitches: frames taking over 100 ms once the game is running.
            auto now = std::chrono::steady_clock::now();
            double frameTime = std::chrono::duration<double, std::milli>(now - lastPresent).count();
            lastPresent = now;
            static uint32_t lastCreated, lastDraws;
            static double lastDrawTime, lastWait, lastPrepare, lastTextures;
            if (frameTime > 100.0 && g.frameNumber > 120)
                LOGF_INFO("hitch: tick {} frame {} took {:.0f} ms ({} pipelines created, {} draws in {:.1f} ms, textures {:.1f} ms, prepare {:.1f} ms, "
                    "GPU wait {:.1f} ms)", uint32_t(GetTickCount() & 0x7FFFFFFF), g.frameNumber, frameTime, g.pipelinesCreated - lastCreated, g_profile.draws - lastDraws,
                    g_profile.draw - lastDrawTime, g_profile.textures - lastTextures, g_profile.prepare - lastPrepare,
                    g_profile.presentWait - lastWait);
            lastCreated = g.pipelinesCreated;
            lastDraws = g_profile.draws;
            lastDrawTime = g_profile.draw;
            lastWait = g_profile.presentWait;
            lastPrepare = g_profile.prepare;
            lastTextures = g_profile.textures;
        }
        static auto lastReport = std::chrono::steady_clock::now();
        if (g.frameNumber % 300 == 150)
            SavePipelineCache();
        if (g.frameNumber % 300 == 0 && g.frameNumber != 0 && (AR_ENV("AR_PROFILE") || AR_ENV("AR_LOG_FPS")))
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(now - lastReport).count();
            lastReport = now;
            LOGF_INFO("profile: {:.1f} fps; per frame {:.2f} ms draws ({:.0f} draws, textures {:.2f}, vertices {:.2f}, constants {:.2f}, "
                "indices {:.2f}, pipeline {:.2f}, targets {:.2f}, setup {:.2f}, record {:.2f}, prepare {:.2f}, flushes {}, vertex MB {:.1f}; texture lookups {} misses {} uploads {} hash {:.2f} decode {:.2f}), "
                "pipelines created {} loaded {}, present {:.2f} (waiting {:.2f})",
                300000.0 / elapsed, g_profile.draw / 300, g_profile.draws / 300.0, g_profile.textures / 300, g_profile.vertices / 300,
                g_profile.constants / 300, g_profile.indices / 300, g_profile.pipeline / 300, g_profile.targets / 300, g_profile.setup / 300, g_profile.record / 300, g_profile.prepare / 300, g_profile.flushes, g_profile.vertexBytes / 300.0 / 1048576.0, g_profile.textureLookups / 300,
                g_profile.textureMisses / 300, g_profile.textureUploads, g_profile.textureHash / 300, g_profile.textureDecode / 300, g.pipelinesCreated, g.pipelinesLoaded,
                g_profile.present / 300, g_profile.presentWait / 300);
            if (g.timestampHeap)
            {
                const double* scopes = g_profile.gpuScopes;
                LOGF_INFO("GPU time per frame: {:.2f} ms (layout conversions {:.2f}, render target syncs {:.2f}, resolves {:.2f}, "
                    "composite textures {:.2f}, draws and the rest {:.2f})", g_profile.gpu / 300, scopes[0] / 300, scopes[1] / 300,
                    scopes[2] / 300, scopes[3] / 300, (g_profile.gpu - scopes[0] - scopes[1] - scopes[2] - scopes[3]) / 300);
            }
            g_profile = {};
        }

        ScopedTimer presentTimer{ g_profile.present };

        TextureFetch fetch;
        memcpy(fetch.words, swap.fetchConstant, sizeof(fetch.words));
        uint32_t frontBuffer = fetch.words[1] != 0 ? fetch.BaseAddress() : swap.frontBuffer;
        uint32_t frontBufferRow;
        ResolvedTexture* frontBufferTexture = FindResolved(frontBuffer, 0, frontBufferRow);
        auto resolved = g.resolvedTextures.end();
        for (auto it = g.resolvedTextures.begin(); it != g.resolvedTextures.end(); ++it)
        {
            if (&it->second == frontBufferTexture)
                resolved = it;
        }
        if (AR_ENV("AR_LOG_PRESENT") && (g.frameNumber % 150 == 0 || (g.frameNumber >= 300 && g.frameNumber < 303)))
            LOGF_INFO("present {}: front buffer 0x{:08X} (packet 0x{:08X}, fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}), {}", g.frameNumber, frontBuffer,
                swap.frontBuffer, fetch.words[0], fetch.words[1], fetch.words[2], fetch.words[3], fetch.words[4], fetch.words[5], resolved != g.resolvedTextures.end() ? "found" : "not resolved");

        ID3D12Resource* backBuffer = g.backBuffers[g.frameIndex].Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->ResourceBarrier(1, &barrier);

        auto rtv = CpuRtv(g.backBufferRtvs[g.frameIndex]);
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g.list->ClearRenderTargetView(rtv, black, 0, nullptr);

        static const uint64_t screenshotInterval = AR_ENV("AR_SCREENSHOT_INTERVAL") ? std::max(1, atoi(AR_ENV("AR_SCREENSHOT_INTERVAL"))) : 300;
        if (resolved != g.resolvedTextures.end() && !g_screenshotFolder.empty() && g.frameNumber % screenshotInterval == 0 &&
            resolved->second.texture.format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            SaveScreenshot(resolved->second.texture, (swap.width ? swap.width : 1280) * g_scale, (swap.height ? swap.height : 720) * g_scale,
                g_screenshotFolder / fmt::format("frame_{:06}.bmp", g.frameNumber), fetch.words[1] != 0 && (fetch.Swizzle() & 7) == 2);
        }
        // The picture of the controller's diagram: once the loading screen has faded in, with the original art for two frames.
        {
            static bool haveDiagram = std::filesystem::exists(CONTROLLER_DIAGRAM_FILE);
            static uint64_t firstSeen = UINT64_MAX, captureAt = UINT64_MAX;
            static auto firstSeenTime = std::chrono::steady_clock::time_point{};
            if (!haveDiagram)
            {
                bool seen = g_diagramSeenFrame != UINT64_MAX && g.frameNumber - g_diagramSeenFrame < 5;
                if (!seen)
                {
                    firstSeen = UINT64_MAX;
                    g_diagramCapturing = false;
                }
                else if (firstSeen == UINT64_MAX)
                {
                    firstSeen = g.frameNumber;
                    firstSeenTime = std::chrono::steady_clock::now();
                }
                else if (!g_diagramCapturing && std::chrono::steady_clock::now() - firstSeenTime > std::chrono::milliseconds(1500))
                {
                    g_diagramCapturing = true;
                    captureAt = g.frameNumber + 3;
                }
                else if (g_diagramCapturing && g.frameNumber >= captureAt && resolved != g.resolvedTextures.end() &&
                    resolved->second.texture.format == DXGI_FORMAT_R8G8B8A8_UNORM)
                {
                    SaveScreenshot(resolved->second.texture, (swap.width ? swap.width : 1280) * g_scale, (swap.height ? swap.height : 720) * g_scale,
                        CONTROLLER_DIAGRAM_FILE, fetch.words[1] != 0 && (fetch.Swizzle() & 7) == 2);
                    LOGF_INFO("saved the controller diagram to {}", CONTROLLER_DIAGRAM_FILE);
                    haveDiagram = true;
                    g_diagramCapturing = false;
                }
            }
        }

        if (resolved != g.resolvedTextures.end() && resolved->second.texture.format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            std::string request;
            {
                std::lock_guard lock(g_screenshotRequestMutex);
                request.swap(g_screenshotRequest);
            }
            if (!request.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories("screenshots", ec);
                SaveScreenshot(resolved->second.texture, (swap.width ? swap.width : 1280) * g_scale, (swap.height ? swap.height : 720) * g_scale,
                    fmt::format("screenshots/{}.bmp", request), fetch.words[1] != 0 && (fetch.Swizzle() & 7) == 2);
                LOGF_INFO("remote: saved screenshots/{}.bmp at frame {}", request, g.frameNumber);
            }
        }

        if (resolved != g.resolvedTextures.end())
        {
            // Scaled to fit the window, keeping the aspect ratio.
            float sourceWidth = float(swap.width ? swap.width : 1280), sourceHeight = float(swap.height ? swap.height : 720);
            float scale = std::min(float(g.swapWidth) / sourceWidth, float(g.swapHeight) / sourceHeight);
            float viewWidth = sourceWidth * scale, viewHeight = sourceHeight * scale;
            float viewX = (float(g.swapWidth) - viewWidth) * 0.5f, viewY = (float(g.swapHeight) - viewHeight) * 0.5f;

            // The source texture can be larger than the displayed area.
            struct PresentConstants
            {
                float origin[2];
                float size[2];
                uint32_t sourceIndex;
                uint32_t samplerIndex;
                uint32_t swapRedBlue;
            } constants{ { viewX, viewY },
                { viewWidth * float(resolved->second.texture.width) / (sourceWidth * g_scale),
                    viewHeight * float(resolved->second.texture.height) / (sourceHeight * g_scale) },
                resolved->second.texture.srv, GetLinearClampSampler(), fetch.words[1] != 0 && (fetch.Swizzle() & 7) == 2 ? 1u : 0u };

            Transition(resolved->second.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT viewport{ viewX, viewY, viewWidth, viewHeight, 0.0f, 1.0f };
            D3D12_RECT scissor{ LONG(viewX), LONG(viewY), LONG(viewX + viewWidth), LONG(viewY + viewHeight) };
            g.list->RSSetViewports(1, &viewport);
            g.list->RSSetScissorRects(1, &scissor);
            g.list->SetPipelineState(GetBlitPipeline(DXGI_FORMAT_R8G8B8A8_UNORM, true, false));
            g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
            g.list->DrawInstanced(3, 1, 0, 0);

            DrawOverlay(viewX, viewY, viewWidth / 1280.0f);
        }

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        g.list->ResourceBarrier(1, &barrier);
        if (g.occlusionHeap && !g.occlusionLabels[g.frameIndex].empty())
        {
            uint32_t base = g.frameIndex * State::OCCLUSIONS_PER_FRAME;
            uint32_t used = uint32_t(g.occlusionLabels[g.frameIndex].size());
            g.list->ResolveQueryData(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, base, used, g.occlusionReadback.Get(), uint64_t(base) * 8);
            g.list->ResolveQueryData(g.occlusionHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, base + State::PROBE_OFFSET, used,
                g.occlusionReadback.Get(), uint64_t(base + State::PROBE_OFFSET) * 8);
            if (g.statsHeap)
                g.list->ResolveQueryData(g.statsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, base, used, g.statsReadback.Get(),
                    uint64_t(base) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        }
        g.shadowResolves = 0;
        if (g.timestampHeap)
        {
            uint32_t base = g.frameIndex * State::TIMESTAMPS_PER_FRAME;
            g.list->EndQuery(g.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
            g.list->ResolveQueryData(g.timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, g.timestampsUsed[g.frameIndex],
                g.timestampReadback.Get(), uint64_t(base) * 8);
            g.timestampsPending[g.frameIndex] = true;
        }
        Submit();
        g_slotCursor[g.frameIndex] = g.uploadOffset;
        g.list->Present(0, g.tearing && !g.exclusiveFullscreen ? DXGI_PRESENT_ALLOW_TEARING : 0);
        // Experiment (AR_SERIALIZE_FRAMES): the GPU finishes each frame before the next one is recorded.
        static const bool serialize = AR_ENV("AR_SERIALIZE_FRAMES") != nullptr;
        if (serialize)
            WaitForFence(g.fenceValue);
        // Flip model swap chains go through their buffers in order.
        g.frameIndex = (g.frameIndex + 1) % FRAME_COUNT;

        uint32_t width, height;
        window::GetBufferSize(width, height);
        if (width != g.swapWidth || height != g.swapHeight)
        {
            g.list.WaitIdle();
            WaitForFence(g.fenceValue);
            for (auto& buffer : g.backBuffers)
                buffer.Reset();
            g.swapChain->ResizeBuffers(FRAME_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
            g.swapWidth = width;
            g.swapHeight = height;
            CreateSwapChainBuffers();
            g.frameIndex = g.swapChain->GetCurrentBackBufferIndex();
        }

        g.frameNumber++;
        BeginFrame();
    }
}
