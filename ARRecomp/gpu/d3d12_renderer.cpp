#include <stdafx.h>
#include "d3d12_renderer.h"
#include "shader_compiler.h"
#include "texture_decode.h"
#include "window.h"
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
    constexpr uint64_t UPLOAD_SIZE = 96ull << 20;
    constexpr uint32_t VIEW_HEAP_SIZE = 65536;
    constexpr uint32_t SAMPLER_HEAP_SIZE = 2048;
    constexpr uint32_t RTV_HEAP_SIZE = 1024;
    constexpr uint32_t DSV_HEAP_SIZE = 256;
    constexpr uint32_t RENDER_TARGET_HEIGHT = 2048;
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
        float padding[2];
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
    };

    struct CachedTexture
    {
        Texture texture;
        uint64_t contentHash = 0;
        uint64_t checkedFrame = UINT64_MAX;
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
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        uint64_t fenceValue = 0;

        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12Resource> backBuffers[FRAME_COUNT];
        uint32_t backBufferRtvs[FRAME_COUNT];
        uint32_t swapWidth = 0, swapHeight = 0;
        bool tearing = false;

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

        ComPtr<ID3D12DescriptorHeap> viewHeap, samplerHeap, rtvHeap, dsvHeap;
        uint32_t viewSize = 0, samplerSize = 0, rtvSize = 0, dsvSize = 0;
        DescriptorAllocator views, rtvs, dsvs;
        uint32_t nullRtv = 0;
        uint32_t samplerCount = 0;
        std::unordered_map<uint32_t, uint32_t> samplers;

        ComPtr<ID3D12RootSignature> rootSignature;
        std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
        std::vector<uint8_t> rectangleGeometryShader, pointGeometryShader;
        std::vector<uint8_t> blitVertexShader, blitPixelShader, presentPixelShader, depthBlitPixelShader, depthCopyPixelShader,
            colorToDepthPixelShader;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> colorToDepthPipelines;
        bool stencilRefSupported = false;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> depthCopyPipelines;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> depthBlitPipelines;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> blitPipelines;
        ComPtr<ID3D12PipelineState> presentPipeline;

        std::unordered_map<RenderTargetKey, Texture, RenderTargetKeyHash> renderTargets;
        // When each render target was last used: overlapping ones copy from those used after them.
        std::unordered_map<RenderTargetKey, uint64_t, RenderTargetKeyHash> renderTargetStamps;
        // Regions recently written in each render target (in samples), with the stamp they were written under.
        std::unordered_map<RenderTargetKey, std::vector<std::pair<uint64_t, D3D12_RECT>>, RenderTargetKeyHash> renderTargetWrites;
        uint64_t renderTargetClock = 0;
        std::unordered_map<uint32_t, ResolvedTexture> resolvedTextures;
        std::unordered_map<uint64_t, CachedTexture> textures;
        std::set<uint32_t> warnedFormats;
    } g;

    // Time spent on the command processor thread, logged every 300 frames.
    struct Profile
    {
        double draw, textures, vertices, present, presentWait;
        uint32_t draws;
    } static g_profile;

    struct ScopedTimer
    {
        double& target;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~ScopedTimer() { target += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
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
        if (texture.resource)
            frame.releasedResources.push_back(std::move(texture.resource));
        if (texture.srv != UINT32_MAX)
            frame.releasedSrvs.push_back(texture.srv);
        if (texture.stencilSrv != UINT32_MAX)
            frame.releasedSrvs.push_back(texture.stencilSrv);
        g.rtvs.Free(texture.rtv);
        g.dsvs.Free(texture.dsv);
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

    static void BindHeaps()
    {
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

    static void BeginFrame()
    {
        auto& frame = g.frames[g.frameIndex];
        {
            ScopedTimer timer{ g_profile.presentWait };
            WaitForFence(frame.fenceValue);
        }
        frame.releasedResources.clear();
        for (uint32_t srv : frame.releasedSrvs)
            g.views.Free(srv);
        frame.releasedSrvs.clear();

        frame.allocator->Reset();
        g.list->Reset(frame.allocator.Get(), nullptr);
        g.uploadOffset = g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT);
        g.vertexUploads.clear();
        BindHeaps();
    }

    static void Submit()
    {
        g.list->Close();
        ID3D12CommandList* lists[] = { g.list.Get() };
        g.queue->ExecuteCommandLists(1, lists);
        g.queue->Signal(g.fence.Get(), ++g.fenceValue);
        g.frames[g.frameIndex].fenceValue = g.fenceValue;
    }

    // Submits the work so far and waits for it, so the frame's upload space can be reused.
    static void FlushAndWait()
    {
        Submit();
        WaitForFence(g.fenceValue);
        auto& frame = g.frames[g.frameIndex];
        frame.allocator->Reset();
        g.list->Reset(frame.allocator.Get(), nullptr);
        g.uploadOffset = g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT);
        g.vertexUploads.clear();
        BindHeaps();
    }

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
};

Texture2D<float4> textures[] : register(t0, space0);

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 coords = (int2(position.xy) + sourceOffset) * sampleScale;
    float4 color = 0.0f;
    for (int y = 0; y < sampleScale.y; y++)
        for (int x = 0; x < sampleScale.x; x++)
            color += textures[NonUniformResourceIndex(sourceIndex)].Load(int3(coords + int2(x, y), 0));
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
    int3 coords = int3((int2(position.xy) + sourceOffset) * sampleScale, 0);
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
};

Texture2D<float4> textures[] : register(t0, space0);

float main(float4 position : SV_Position
#ifdef STENCIL_REF
    , out uint stencil : SV_StencilRef
#endif
    ) : SV_Depth
{
    uint4 bytes = uint4(round(saturate(textures[NonUniformResourceIndex(colorIndex)].Load(int3(int2(position.xy) + sourceOffset, 0))) * 255.0f));
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
        HWND hwnd = window::Create(1280, 720, "Anarchy Reigns");

        if (getenv("AR_D3D12_DEBUG"))
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
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

        if (adapter)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            std::wstring name = desc.Description;
            LOGF_INFO("Direct3D 12 adapter: {}", std::string(name.begin(), name.end()));
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
            LOGF_WARNING("the GPU only supports resource binding tier 1; rendering may fail");
        g.stencilRefSupported = options.PSSpecifiedStencilRefSupported;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));

        for (auto& frame : g.frames)
            g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
        g.list->Close();
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
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = UPLOAD_SIZE;
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

        if (!CreateRootSignature())
            return false;

        g.rectangleGeometryShader = CompileInternal(RECTANGLE_GS, L"gs_6_0", "rectangle_gs");
        g.pointGeometryShader = CompileInternal(POINT_GS, L"gs_6_0", "point_gs");
        g.blitVertexShader = CompileInternal(BLIT_VS, L"vs_6_0", "blit_vs");
        g.blitPixelShader = CompileInternal(BLIT_PS, L"ps_6_0", "blit_ps");
        g.presentPixelShader = CompileInternal(PRESENT_PS, L"ps_6_0", "present_ps");
        g.depthBlitPixelShader = CompileInternal(DEPTH_BLIT_PS, L"ps_6_0", "depth_blit_ps");
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

        window::GetClientSize(g.swapWidth, g.swapHeight);
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
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

        g.frameIndex = g.swapChain->GetCurrentBackBufferIndex();
        g.initialized = true;
        g.failed = false;
        BeginFrame();
        return true;
    }

    static bool TraceThisFrame();
    static void SaveScreenshot(struct Texture& texture, uint32_t width, uint32_t height, const std::filesystem::path& path,
        bool swapRedBlue = false);

    // ---- Textures ----

    static Texture CreateTexture(uint32_t width, uint32_t height, uint32_t depth, bool cube, DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        Texture texture;
        texture.format = format;
        texture.width = width;
        texture.height = height;

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
        }

        return texture;
    }

    static uint64_t HashMemory(const uint8_t* data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        size_t i = 0;
        for (; i + 8 <= size; i += 8)
        {
            uint64_t value;
            memcpy(&value, data + i, 8);
            hash = (hash ^ value) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        for (; i < size; i++)
            hash = (hash ^ data[i]) * 0x100000001B3ull;
        return hash;
    }

    static void UploadTexture(Texture& texture, const DecodedTexture& decoded)
    {
        uint64_t sliceSize = uint64_t(decoded.rowPitch) * decoded.rowCount;
        auto allocation = Upload(sliceSize * decoded.depth, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
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
            uint64_t end = uint64_t(base) + uint64_t(resolved.rowBytes) * resolved.texture.height;
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
    static uint32_t GetTexture(const TextureFetch& fetch, uint32_t dimension, float* transform, uint32_t& rawDepth)
    {
        rawDepth = 0;
        transform[0] = transform[1] = 1.0f;
        transform[2] = transform[3] = 0.0f;
        uint32_t nullSrv = dimension == 3 ? NULL_SRV_CUBE : dimension == 2 ? NULL_SRV_3D : NULL_SRV_2D;
        if ((fetch.words[0] & 3) != 2) // Not a texture fetch constant.
            return nullSrv;
        static const bool nullCubes = getenv("AR_NULL_CUBE") != nullptr;
        if (nullCubes && dimension == 3)
            return nullSrv;

        // Resolved render targets are used directly.
        uint32_t row;
        ResolvedTexture* resolved = dimension < 2 ? FindResolved(fetch.BaseAddress(), 0, row) : nullptr;
        if (resolved)
        {
            auto& texture = resolved->texture;
            transform[0] = float(fetch.Width()) / float(texture.width);
            transform[1] = float(fetch.Height()) / float(texture.height);
            transform[3] = float(row) / float(texture.height);
            if (resolved->rawDepth && (fetch.Format() == 22 || fetch.Format() == 23))
                rawDepth = fetch.Format() == 23 ? 2 : 1;
            Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return texture.srv;
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
            uint64_t hash = HashMemory(g_memory.Translate<uint8_t>(PhysicalToVirtual(fetch.BaseAddress())), size);
            if (!entry.texture.resource || hash != entry.contentHash)
            {
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

                bool firstUpload = entry.contentHash == 0;
                static int changeLogs;
                if (!firstUpload && getenv("AR_TRACE_FRAME") && changeLogs++ < 40)
                    LOGF_INFO("texture {:08X} changed in frame {}", fetch.BaseAddress(), g.frameNumber);
                UploadTexture(entry.texture, decoded);
                entry.contentHash = hash;

                if (TraceThisFrame() || (getenv("AR_TRACE_FRAME") && firstUpload))
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

    static D3D12_RECT UnionRect(const D3D12_RECT& a, const D3D12_RECT& b)
    {
        return { std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom) };
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

        std::vector<std::pair<uint64_t, std::pair<const RenderTargetKey*, Texture*>>> newer;
        for (auto& [otherKey, other] : g.renderTargets)
        {
            // 8888 color targets also take the raw words of depth buffers over them (games clear color by clearing
            // depth at the same EDRAM address).
            bool depthIntoColor = !key.depth && otherKey.depth && (key.format == 0 || key.format == 1);
            bool colorIntoDepth = key.depth && !otherKey.depth && (otherKey.format == 0 || otherKey.format == 1);
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

        for (auto& [otherStamp, entry] : newer)
        {
            auto& [otherKey, other] = entry;
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

                if (key.depth)
                {
                    Transition(*other, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    Transition(texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    auto dsv = CpuDsv(texture.dsv);
                    g.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                    SetViewportAndScissor(texture.width, texture.height, written);
                    g.list->SetPipelineState(GetDepthCopyPipeline(texture.format, true));
                    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    struct
                    {
                        int32_t sourceOffset[2];
                        uint32_t colorIndex;
                        uint32_t floatDepth;
                    } constants{ { 0, int32_t(sourceTop) - int32_t(destTop) }, other->srv, key.format == 1 ? 1u : 0u };
                    g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
                    g.list->DrawInstanced(3, 1, 0, 0);
                    continue;
                }

                Transition(*other, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto rtv = CpuRtv(texture.rtv);
                g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                SetViewportAndScissor(texture.width, texture.height, written);
                g.list->SetPipelineState(GetBlitPipeline(texture.format, false, true));
                g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                struct
                {
                    int32_t sourceOffset[2];
                    uint32_t depthIndex;
                    uint32_t stencilIndex;
                    uint32_t floatDepth;
                    int32_t sampleScale[2];
                } constants{ { 0, int32_t(sourceTop) - int32_t(destTop) }, other->srv, other->stencilSrv,
                    otherKey->format == 1 ? 1u : 0u, { 1, 1 } };
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
                    { 0, LONG(destTop), LONG(texture.width), LONG(destTop + height) });
                g.list->SetPipelineState(GetDepthCopyPipeline(texture.format));
                g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                struct
                {
                    int32_t sourceOffset[2];
                    uint32_t depthIndex;
                    uint32_t stencilIndex;
                } constants{ { 0, int32_t(sourceTop) - int32_t(destTop) }, other->srv, other->stencilSrv };
                g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, UploadConstants(constants));
                g.list->DrawInstanced(3, 1, 0, 0);
            }
            else
            {
                Transition(*other, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION source{ other->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION dest{ texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_BOX box{ 0, sourceTop, 0, texture.width, sourceTop + height, 1 };
                g.list->CopyTextureRegion(&dest, 0, destTop, 0, &source, &box);
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
        Texture texture = CreateTexture(pitch, RENDER_TARGET_HEIGHT, 1, false, hostFormat,
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

    static ID3D12PipelineState* GetPipeline(const PipelineKey& key, const HostShader& vertexShader, const HostShader* pixelShader)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(key); i++)
            hash = (hash ^ reinterpret_cast<const uint8_t*>(&key)[i]) * 0x100000001B3ull;

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
        desc.RasterizerState.DepthClipEnable = !((key.rasterizer >> 3) & 1) && !getenv("AR_NO_DEPTH_CLIP");

        uint32_t depth = key.depthControl;
        auto& depthStencil = desc.DepthStencilState;
        depthStencil.DepthEnable = (depth >> 1) & 1;
        depthStencil.DepthWriteMask = ((depth >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencil.DepthFunc = getenv("AR_NO_DEPTH_TEST") ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC(((depth >> 4) & 7) + 1);
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

        if (FAILED(g.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
        {
            LOGF_ERROR("failed to create a pipeline for shaders {:016X} / {:016X}", vertexShader.hash, pixelShader ? pixelShader->hash : 0);
            return nullptr;
        }

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

    // Logs every draw of one frame, for debugging (frame number from AR_TRACE_FRAME).
    bool TraceThisFrame()
    {
        static int64_t frame = getenv("AR_TRACE_FRAME") ? atoll(getenv("AR_TRACE_FRAME")) : -1;
        return int64_t(g.frameNumber) == frame;
    }

    void Draw(const be<uint32_t>* registers, const DrawCall& draw, const HostShader& vertexShader, const HostShader* pixelShader)
    {
        if (!Initialize())
            return;

        ScopedTimer timer{ g_profile.draw };
        g_profile.draws++;

        auto mode = EdramMode(registers[RB_MODECONTROL] & 7);
        if (mode == EdramMode::Copy)
        {
            Resolve(registers);
            return;
        }

        if (mode != EdramMode::ColorDepth && mode != EdramMode::DepthOnly)
            return;

        // Debugging (AR_SKIP_PS=<hex hash prefixes, comma separated>): draws with these pixel shaders are skipped.
        static const std::vector<std::string> skippedShaders = []
            {
                std::vector<std::string> list;
                if (const char* text = getenv("AR_SKIP_PS"))
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
                    // Raw depth words can't be filtered.
                    TextureFetch samplerFetch = fetch;
                    if (rawDepth)
                        samplerFetch.words[3] &= ~(0xFu << 19);
                    slot[1] = GetSampler(samplerFetch);
                    slot[2] = fetch.Swizzle();
                    slot[3] = signs | (rawDepth << 8);
                }
            };
        {
            ScopedTimer textureTimer{ g_profile.textures };
            bindTextures(vertexShader);
            if (pixelShader)
                bindTextures(*pixelShader);
        }

        // Index data.
        std::vector<uint32_t> indices;
        bool indexed = draw.initiator.Source() != SourceSelect::AutoIndex;
        bool primitiveReset = ((modeControl >> 21) & 1) && (topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP || topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
        if (conversion != Conversion::None || (indexed && (primitiveReset || draw.initiator.Source() == SourceSelect::Immediate)))
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

        // Vertex data: every vertex buffer the shader fetches from is copied into the upload buffer.
        uint64_t frameBase = g.frameIndex * (UPLOAD_SIZE / FRAME_COUNT);
        uint64_t uploadSize = 3 * 4096 + indices.size() * 4 + size_t(count) * 4;
        std::vector<std::pair<uint32_t, uint32_t>> vertexRanges;
        for (const auto& binding : vertexShader.shader->vertex_bindings())
        {
            uint32_t index = binding.fetch_constant;
            uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
            uint32_t size = ((registers[SHADER_CONSTANT_FETCH_00_0 + index * 2 + 1] >> 2) & 0xFFFFFF) * 4;
            vertexRanges.emplace_back(index, size);
            uploadSize += size + 16;
        }

        if (g.uploadOffset + uploadSize > frameBase + UPLOAD_SIZE / FRAME_COUNT)
            FlushAndWait();

        ScopedTimer vertexTimer{ g_profile.vertices };
        for (auto [index, size] : vertexRanges)
        {
            if (size == 0)
                continue;
            uint32_t address = registers[SHADER_CONSTANT_FETCH_00_0 + index * 2] & 0x1FFFFFFC;
            auto& uploaded = g.vertexUploads[(uint64_t(address) << 32) | size];
            if (uploaded == 0)
            {
                auto allocation = Upload(size, 16);
                memcpy(allocation.cpu, g_memory.Translate(PhysicalToVirtual(address)), size);
                uploaded = allocation.offset + 1;
            }
            system.vertexDataOffsets[index] = int32_t(uploaded - 1 - frameBase) - int32_t(address);
        }

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
        static const int highlightPrimitive = getenv("AR_HIGHLIGHT_PRIM") ? atoi(getenv("AR_HIGHLIGHT_PRIM")) : -1;
        if (int(draw.initiator.PrimType()) == highlightPrimitive)
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

        // Only the constants up to the highest one the shader reads are copied (all of them when it indexes them).
        auto uploadFloatConstants = [&](uint32_t baseRegister, const HostShader* shader)
            {
                uint32_t count = 0;
                if (shader)
                {
                    const auto& map = shader->shader->constant_register_map();
                    if (map.float_dynamic_addressing)
                        count = 256;
                    for (uint32_t i = 0; i < 4; i++)
                    {
                        if (map.float_bitmap[i])
                            count = std::max<uint32_t>(count, i * 64 + 64 - uint32_t(std::countl_zero(map.float_bitmap[i])));
                    }
                }

                uint32_t base = std::min<uint32_t>(baseRegister & 0x1FF, 256);
                auto allocation = Upload(4096, 256);
                auto* out = reinterpret_cast<uint32_t*>(allocation.cpu);
                for (uint32_t i = 0; i < count * 4; i++)
                    out[i] = registers[SHADER_CONSTANT_000_X + base * 4 + i];
                return allocation.gpu;
            };
        D3D12_GPU_VIRTUAL_ADDRESS vertexConstants = uploadFloatConstants(registers[0x2307], &vertexShader);
        D3D12_GPU_VIRTUAL_ADDRESS pixelConstants = uploadFloatConstants(registers[0x2308], pixelShader);
        D3D12_GPU_VIRTUAL_ADDRESS systemConstants = UploadConstants(system);

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
                for (uint32_t i = 0; i < count; i++)
                {
                    uint16_t value;
                    memcpy(&value, source + i * 2, 2);
                    value = draw.indexEndian == 1 ? ByteSwap(value) : value;
                    memcpy(allocation.cpu + i * 2, &value, 2);
                }
            }
            indexView = { allocation.gpu, size, index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
        }

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
                    if (getenv("AR_TRACE_FETCH"))
                        textures += fmt::format("[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", fetch.words[0], fetch.words[1],
                            fetch.words[2], fetch.words[3], fetch.words[4], fetch.words[5]);
                }
            }
            LOGF_INFO("draw prim {} count {} indexed {} vs {:016X} ps {:016X} rt {} mask {:04X} depth {:08X} pitch {} scissor {},{}-{},{} ndc {:.4f},{:.4f},{:.4f} / {:.4f},{:.4f},{:.4f} vte {:X} blend {:08X}{}",
                uint32_t(draw.initiator.PrimType()), count, indexed, key.vertexShader, key.pixelShader, key.renderTargetCount,
                colorMask, depthControl, pitch, scissor.left, scissor.top, scissor.right, scissor.bottom,
                system.ndcScale[0], system.ndcScale[1], system.ndcScale[2], system.ndcOffset[0], system.ndcOffset[1], system.ndcOffset[2],
                vte, key.blend[0], textures);
            LOGF_INFO("  line {:08X} point {:08X} {:08X}", uint32_t(registers[0x2282]), uint32_t(registers[PA_SU_POINT_SIZE]),
                uint32_t(registers[PA_SU_POINT_MINMAX]));
            LOGF_INFO("  surface {:08X} color {:08X} depth {:08X} window {:08X} vport {} {} {} {}", uint32_t(registers[RB_SURFACE_INFO]),
                uint32_t(registers[RB_COLOR_INFO]), uint32_t(registers[RB_DEPTH_INFO]), uint32_t(registers[PA_SC_WINDOW_OFFSET]),
                RegisterFloat(registers, PA_CL_VPORT_XSCALE), RegisterFloat(registers, PA_CL_VPORT_XOFFSET),
                RegisterFloat(registers, PA_CL_VPORT_YSCALE), RegisterFloat(registers, PA_CL_VPORT_YOFFSET));
        }

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
        g.list->OMSetRenderTargets(key.renderTargetCount, rtvs, FALSE, depthTarget ? &dsv : nullptr);
        SetViewportAndScissor(pitch, RENDER_TARGET_HEIGHT, ScaleRect(GetScissor(registers), sampleScale));

        g.list->SetPipelineState(pipeline);
        g.list->IASetPrimitiveTopology(topology);
        g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS, systemConstants);
        g.list->SetGraphicsRootConstantBufferView(ROOT_VERTEX_FLOAT_CONSTANTS, vertexConstants);
        g.list->SetGraphicsRootConstantBufferView(ROOT_PIXEL_FLOAT_CONSTANTS, pixelConstants);
        g.list->SetGraphicsRootShaderResourceView(ROOT_VERTEX_DATA, g.uploadGpu + frameBase);

        float blendFactor[4] = { RegisterFloat(registers, RB_BLEND_RED), RegisterFloat(registers, RB_BLEND_GREEN),
            RegisterFloat(registers, RB_BLEND_BLUE), RegisterFloat(registers, RB_BLEND_ALPHA) };
        g.list->OMSetBlendFactor(blendFactor);
        g.list->OMSetStencilRef(stencilRefMask & 0xFF);

        if (indexed)
        {
            g.list->IASetIndexBuffer(&indexView);
            g.list->DrawIndexedInstanced(count, 1, 0, 0, 0);
        }
        else
        {
            g.list->DrawInstanced(count, 1, 0, 0);
        }
    }

    // ---- Resolves ----

    static void Resolve(const be<uint32_t>* registers)
    {
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

        D3D12_RECT rect{ x0, y0, x1, y1 };
        uint32_t destBase = registers[RB_COPY_DEST_BASE] & 0x1FFFFFFF;
        if (getenv("AR_LOG_PRESENT") && g.frameNumber >= 300 && g.frameNumber < 303 && destBase == 0x0CEF0000)
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
        if (getenv("AR_LOG_RESOLVES"))
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
                if (target && (target->texture.format != destFormat || target->texture.width != destWidth ||
                    row + uint32_t(y1) > target->texture.height || (row == 0 && target->texture.height != destHeight)))
                {
                    target = nullptr;
                }

                if (!target)
                {
                    ResolvedTexture created;
                    created.texture = CreateTexture(destWidth, destHeight, 1, false, destFormat,
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
                            if (inside && inner.texture.format == destFormat && inner.texture.width == destWidth &&
                                innerRow + inner.texture.height <= destHeight)
                            {
                                Transition(inner.texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
                                Transition(created.texture, D3D12_RESOURCE_STATE_COPY_DEST);
                                D3D12_TEXTURE_COPY_LOCATION source{ inner.texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                D3D12_TEXTURE_COPY_LOCATION dest{ created.texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                g.list->CopyTextureRegion(&dest, 0, innerRow, 0, &source, nullptr);
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
                    struct BlitConstants
                    {
                        int32_t sourceOffset[2];
                        uint32_t sourceIndex;
                        uint32_t swapRedBlue;
                        float scale[4];
                        int32_t sampleScale[2];
                    } constants{ { 0, -int32_t(row) }, source->srv, destInfo.Swap() ? 1u : 0u };
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
                    } depthConstants{ { 0, -int32_t(row) }, source->srv, source->stencilSrv,
                        DepthInfo{ registers[RB_DEPTH_INFO] }.Format() == 1 ? 1u : 0u, { int32_t(sampleScale.x), int32_t(sampleScale.y) } };

                    Transition(*source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    Transition(resolved.texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    auto rtv = CpuRtv(resolved.texture.rtv);
                    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    D3D12_RECT destRect{ rect.left, rect.top + LONG(row), rect.right, rect.bottom + LONG(row) };
                    SetViewportAndScissor(resolved.texture.width, resolved.texture.height, destRect);
                    g.list->SetPipelineState(GetBlitPipeline(destFormat, false, depthSource));
                    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    g.list->SetGraphicsRootConstantBufferView(ROOT_SYSTEM_CONSTANTS,
                        depthSource ? UploadConstants(depthConstants) : UploadConstants(constants));
                    g.list->DrawInstanced(3, 1, 0, 0);

                    if (TraceThisFrame() && source->format == DXGI_FORMAT_R8G8B8A8_UNORM && getenv("AR_DUMP_SOURCES"))
                    {
                        static int sourceIndex;
                        SaveScreenshot(*source, source->width, 1024, fmt::format("screenshots/source_{:02}_{:08X}.bmp", sourceIndex++, destBase));
                    }
                    if (TraceThisFrame() && (destFormat == DXGI_FORMAT_R8G8B8A8_UNORM || destFormat == DXGI_FORMAT_R16G16B16A16_FLOAT))
                    {
                        static int resolveIndex;
                        SaveScreenshot(resolved.texture, destWidth, destHeight, fmt::format("screenshots/resolve_{:02}_{:08X}.bmp", resolveIndex++, destBase));
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
            g.list->ClearRenderTargetView(CpuRtv(source->rtv), color, 1, &sourceRect);
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
                Transition(*depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                g.list->ClearDepthStencilView(CpuDsv(depth->dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    float(clear >> 8) / 16777215.0f, UINT8(clear & 0xFF), 1, &depthRect);
                RecordWrite({ info.Base(), pitch * sampleScale.x, info.Format(), true }, depthRect);
            }
        }
    }

    // ---- Presentation ----

    static std::filesystem::path g_screenshotFolder;

    uint64_t GetFrameNumber()
    {
        return g.frameNumber;
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

        static auto lastReport = std::chrono::steady_clock::now();
        if (g.frameNumber % 300 == 0 && g.frameNumber != 0 && getenv("AR_PROFILE"))
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(now - lastReport).count();
            lastReport = now;
            LOGF_INFO("profile: {:.1f} fps; per frame {:.2f} ms draws ({:.0f} draws, textures {:.2f}, vertices {:.2f}), present {:.2f} (waiting {:.2f})",
                300000.0 / elapsed, g_profile.draw / 300, g_profile.draws / 300.0, g_profile.textures / 300, g_profile.vertices / 300,
                g_profile.present / 300, g_profile.presentWait / 300);
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
        if (getenv("AR_LOG_PRESENT") && (g.frameNumber % 150 == 0 || (g.frameNumber >= 300 && g.frameNumber < 303)))
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

        static const uint64_t screenshotInterval = getenv("AR_SCREENSHOT_INTERVAL") ? std::max(1, atoi(getenv("AR_SCREENSHOT_INTERVAL"))) : 300;
        if (resolved != g.resolvedTextures.end() && !g_screenshotFolder.empty() && g.frameNumber % screenshotInterval == 0 &&
            resolved->second.texture.format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            SaveScreenshot(resolved->second.texture, swap.width ? swap.width : 1280, swap.height ? swap.height : 720,
                g_screenshotFolder / fmt::format("frame_{:06}.bmp", g.frameNumber), fetch.words[1] != 0 && (fetch.Swizzle() & 7) == 2);
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
                { viewWidth * float(resolved->second.texture.width) / sourceWidth, viewHeight * float(resolved->second.texture.height) / sourceHeight },
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
        }

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        g.list->ResourceBarrier(1, &barrier);
        Submit();
        g.swapChain->Present(0, g.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);

        uint32_t width, height;
        window::GetClientSize(width, height);
        if (width != g.swapWidth || height != g.swapHeight)
        {
            WaitForFence(g.fenceValue);
            for (auto& buffer : g.backBuffers)
                buffer.Reset();
            g.swapChain->ResizeBuffers(FRAME_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
            g.swapWidth = width;
            g.swapHeight = height;
            CreateSwapChainBuffers();
        }

        g.frameIndex = g.swapChain->GetCurrentBackBufferIndex();
        g.frameNumber++;
        BeginFrame();
    }
}
