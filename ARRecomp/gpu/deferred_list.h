#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// A D3D12 command list recorded on one thread and replayed into the real command list on another.
//
// The GPU command processor thread does everything that reads guest state (registers, memory) and copies data into
// upload memory; the driver work of recording, submitting and presenting runs on the submission thread, in the same
// order. Commands are copied into fixed size slots (pointer arguments are copied too), batched, and handed over.
// Fence values are chosen when commands are recorded and signaled by the submission thread when it executes them, so
// waiting for a fence also waits for the submission thread to get there.
namespace gpu::d3d12
{
    class DeferredList
    {
    public:
        // Real objects, set up before Start.
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> real;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12Fence* fence = nullptr;
        IDXGISwapChain3* swapChain = nullptr;

        // Starts the submission thread (or records directly into the real list when threaded is false).
        void Start(bool threaded);

        // Waits until every recorded command has been replayed.
        void WaitIdle();

        DeferredList* operator->() { return this; }

        // Command list methods used by the renderer.
        void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rectCount, const D3D12_RECT* rects);
        void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rectCount, const D3D12_RECT* rects);
        void Close();
        void CopyResource(ID3D12Resource* dest, ID3D12Resource* source);
        void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dest, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box);
        void DrawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance);
        void DrawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance);
        void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view);
        void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
        void OMSetBlendFactor(const FLOAT factor[4]);
        void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
        void OMSetStencilRef(UINT reference);
        void RSSetScissorRects(UINT count, const D3D12_RECT* rects);
        void RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports);
        void Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* state);
        void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers);
        void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps);
        void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);
        void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle);
        void SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);
        void SetGraphicsRootSignature(ID3D12RootSignature* signature);
        void SetPipelineState(ID3D12PipelineState* state);

        // Queue work, in order with the commands: execute the list (closed by then) and signal the fence.
        void Execute(uint64_t fenceValue);
        // Queue work, in order with the commands: execute another (closed) command list.
        void ExecuteOther(ID3D12CommandList* other);
        void ResetAllocator(ID3D12CommandAllocator* allocator);
        void Present(UINT syncInterval, UINT flags);
        void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index);
        void SOSetTargets(const D3D12_STREAM_OUTPUT_BUFFER_VIEW* view); // One view, or none when null.
        void CopyBufferRegion(ID3D12Resource* dest, UINT64 destOffset, ID3D12Resource* source, UINT64 sourceOffset, UINT64 size);
        void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index);
        void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* destination, UINT64 offset);

    private:
        enum class Type : uint32_t
        {
            ClearDepth, ClearColor, Close, CopyResource, CopyTexture, DrawIndexed, Draw, IndexBuffer, Topology,
            BlendFactor, RenderTargets, StencilRef, Scissors, Viewports, Reset, Barriers, Heaps, RootCbv, RootTable,
            RootSrv, RootSignature, Pipeline, Execute, ResetAllocator, Present, BeginQuery, EndQuery, ResolveQueryData,
            StreamOutput, CopyBuffer, ExecuteOther,
        };

        // Fixed size command slot: large enough for a texture copy (two copy locations and a box).
        struct Command
        {
            Type type;
            uint32_t count;
            alignas(8) uint8_t data[232];
        };
        static_assert(sizeof(D3D12_TEXTURE_COPY_LOCATION) * 2 + sizeof(D3D12_BOX) + 16 <= 232);

        template <typename T>
        T& Add(Type type, uint32_t count = 0)
        {
            static_assert(sizeof(T) <= sizeof(Command::data));
            Command& command = m_recording.emplace_back();
            command.type = type;
            command.count = count;
            return *new (command.data) T{};
        }

        void Recorded();
        void Replay(const Command& command);
        void Run();

        bool m_threaded = false;
        std::vector<Command> m_recording;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::condition_variable m_idle;
        std::deque<std::vector<Command>> m_batches;
        std::vector<std::vector<Command>> m_free;
        bool m_busy = false;
    };
}
