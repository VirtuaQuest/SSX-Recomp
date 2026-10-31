#include <stdafx.h>
#include "deferred_list.h"
#include <array>

namespace gpu::d3d12
{
    namespace
    {
        constexpr size_t BATCH_SIZE = 256;

        struct ClearDepthData { D3D12_CPU_DESCRIPTOR_HANDLE dsv; D3D12_CLEAR_FLAGS flags; FLOAT depth; UINT8 stencil; D3D12_RECT rects[4]; };
        struct ClearColorData { D3D12_CPU_DESCRIPTOR_HANDLE rtv; FLOAT color[4]; D3D12_RECT rects[4]; };
        struct CopyResourceData { ID3D12Resource* dest; ID3D12Resource* source; };
        struct CopyTextureData { D3D12_TEXTURE_COPY_LOCATION dest, source; D3D12_BOX box; UINT x, y, z; bool hasBox; };
        struct DrawIndexedData { UINT indexCount, instanceCount, startIndex; INT baseVertex; UINT startInstance; };
        struct DrawData { UINT vertexCount, instanceCount, startVertex, startInstance; };
        struct RenderTargetsData { D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]; D3D12_CPU_DESCRIPTOR_HANDLE dsv; BOOL single; bool hasDsv; };
        struct RectsData { D3D12_RECT rects[8]; };
        struct ViewportsData { D3D12_VIEWPORT viewports[8]; };
        struct ResetData { ID3D12CommandAllocator* allocator; ID3D12PipelineState* state; };
        struct BarriersData { D3D12_RESOURCE_BARRIER barriers[6]; };
        struct HeapsData { ID3D12DescriptorHeap* heaps[2]; };
        struct RootAddressData { UINT index; D3D12_GPU_VIRTUAL_ADDRESS address; };
        struct RootTableData { UINT index; D3D12_GPU_DESCRIPTOR_HANDLE handle; };
        struct PresentData { UINT syncInterval, flags; };
        struct StreamOutputData { D3D12_STREAM_OUTPUT_BUFFER_VIEW view; bool set; };
        struct CopyBufferData { ID3D12Resource* dest; UINT64 destOffset; ID3D12Resource* source; UINT64 sourceOffset, size; };
        struct QueryData { ID3D12QueryHeap* heap; D3D12_QUERY_TYPE type; UINT start, count; ID3D12Resource* destination; UINT64 offset; };
    }

    void DeferredList::Start(bool threaded)
    {
        m_threaded = threaded;
        m_recording.reserve(BATCH_SIZE);
        if (threaded)
        {
            std::thread([this]
                {
                    SetThreadDescription(GetCurrentThread(), L"GPU submission");
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                    Run();
                }).detach();
        }
    }

    // Hands the recorded commands to the submission thread once a batch is full (or replays them right away when not
    // threaded).
    void DeferredList::Recorded()
    {
        if (!m_threaded)
        {
            Replay(m_recording.back());
            m_recording.clear();
            return;
        }

        if (m_recording.size() < BATCH_SIZE)
            return;

        std::vector<Command> next;
        {
            std::lock_guard lock(m_mutex);
            m_batches.push_back(std::move(m_recording));
            if (!m_free.empty())
            {
                next = std::move(m_free.back());
                m_free.pop_back();
            }
        }
        m_wake.notify_one();
        m_recording = std::move(next);
        m_recording.clear();
        m_recording.reserve(BATCH_SIZE);
    }

    void DeferredList::WaitIdle()
    {
        if (!m_threaded)
            return;

        {
            std::lock_guard lock(m_mutex);
            if (!m_recording.empty())
                m_batches.push_back(std::move(m_recording));
        }
        m_recording = {};
        m_recording.reserve(BATCH_SIZE);
        m_wake.notify_one();

        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this] { return m_batches.empty() && !m_busy; });
    }

    void DeferredList::Run()
    {
        while (true)
        {
            std::vector<Command> batch;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return !m_batches.empty(); });
                batch = std::move(m_batches.front());
                m_batches.pop_front();
                m_busy = true;
            }

            for (const Command& command : batch)
                Replay(command);

            {
                std::lock_guard lock(m_mutex);
                m_busy = false;
                batch.clear();
                if (m_free.size() < 16)
                    m_free.push_back(std::move(batch));
                if (m_batches.empty())
                    m_idle.notify_all();
            }
        }
    }

    void DeferredList::Replay(const Command& command)
    {
        auto* list = real.Get();
        const uint8_t* data = command.data;
        switch (command.type)
        {
        case Type::ClearDepth:
        {
            auto& d = *reinterpret_cast<const ClearDepthData*>(data);
            list->ClearDepthStencilView(d.dsv, d.flags, d.depth, d.stencil, command.count, command.count ? d.rects : nullptr);
            break;
        }
        case Type::ClearColor:
        {
            auto& d = *reinterpret_cast<const ClearColorData*>(data);
            list->ClearRenderTargetView(d.rtv, d.color, command.count, command.count ? d.rects : nullptr);
            break;
        }
        case Type::Close:
            list->Close();
            break;
        case Type::CopyResource:
        {
            auto& d = *reinterpret_cast<const CopyResourceData*>(data);
            list->CopyResource(d.dest, d.source);
            break;
        }
        case Type::CopyTexture:
        {
            auto& d = *reinterpret_cast<const CopyTextureData*>(data);
            list->CopyTextureRegion(&d.dest, d.x, d.y, d.z, &d.source, d.hasBox ? &d.box : nullptr);
            break;
        }
        case Type::DrawIndexed:
        {
            auto& d = *reinterpret_cast<const DrawIndexedData*>(data);
            list->DrawIndexedInstanced(d.indexCount, d.instanceCount, d.startIndex, d.baseVertex, d.startInstance);
            break;
        }
        case Type::Draw:
        {
            auto& d = *reinterpret_cast<const DrawData*>(data);
            list->DrawInstanced(d.vertexCount, d.instanceCount, d.startVertex, d.startInstance);
            break;
        }
        case Type::IndexBuffer:
            list->IASetIndexBuffer(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(data));
            break;
        case Type::Topology:
            list->IASetPrimitiveTopology(*reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY*>(data));
            break;
        case Type::BlendFactor:
            list->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(data));
            break;
        case Type::RenderTargets:
        {
            auto& d = *reinterpret_cast<const RenderTargetsData*>(data);
            list->OMSetRenderTargets(command.count, command.count ? d.rtvs : nullptr, d.single, d.hasDsv ? &d.dsv : nullptr);
            break;
        }
        case Type::StencilRef:
            list->OMSetStencilRef(*reinterpret_cast<const UINT*>(data));
            break;
        case Type::Scissors:
            list->RSSetScissorRects(command.count, reinterpret_cast<const RectsData*>(data)->rects);
            break;
        case Type::Viewports:
            list->RSSetViewports(command.count, reinterpret_cast<const ViewportsData*>(data)->viewports);
            break;
        case Type::Reset:
        {
            auto& d = *reinterpret_cast<const ResetData*>(data);
            list->Reset(d.allocator, d.state);
            break;
        }
        case Type::Barriers:
            list->ResourceBarrier(command.count, reinterpret_cast<const BarriersData*>(data)->barriers);
            break;
        case Type::Heaps:
            list->SetDescriptorHeaps(command.count, reinterpret_cast<const HeapsData*>(data)->heaps);
            break;
        case Type::RootCbv:
        {
            auto& d = *reinterpret_cast<const RootAddressData*>(data);
            list->SetGraphicsRootConstantBufferView(d.index, d.address);
            break;
        }
        case Type::RootTable:
        {
            auto& d = *reinterpret_cast<const RootTableData*>(data);
            list->SetGraphicsRootDescriptorTable(d.index, d.handle);
            break;
        }
        case Type::RootSrv:
        {
            auto& d = *reinterpret_cast<const RootAddressData*>(data);
            list->SetGraphicsRootShaderResourceView(d.index, d.address);
            break;
        }
        case Type::RootSignature:
            list->SetGraphicsRootSignature(*reinterpret_cast<ID3D12RootSignature* const*>(data));
            break;
        case Type::Pipeline:
            list->SetPipelineState(*reinterpret_cast<ID3D12PipelineState* const*>(data));
            break;
        case Type::ExecuteOther:
        {
            ID3D12CommandList* lists[] = { *reinterpret_cast<ID3D12CommandList* const*>(data) };
            queue->ExecuteCommandLists(1, lists);
            break;
        }
        case Type::Execute:
        {
            ID3D12CommandList* lists[] = { list };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence, *reinterpret_cast<const uint64_t*>(data));
            break;
        }
        case Type::ResetAllocator:
            (*reinterpret_cast<ID3D12CommandAllocator* const*>(data))->Reset();
            break;
        case Type::StreamOutput:
        {
            auto& d = *reinterpret_cast<const StreamOutputData*>(data);
            list->SOSetTargets(0, 1, d.set ? &d.view : nullptr);
            break;
        }
        case Type::CopyBuffer:
        {
            auto& d = *reinterpret_cast<const CopyBufferData*>(data);
            list->CopyBufferRegion(d.dest, d.destOffset, d.source, d.sourceOffset, d.size);
            break;
        }
        case Type::BeginQuery:
        {
            auto& d = *reinterpret_cast<const QueryData*>(data);
            list->BeginQuery(d.heap, d.type, d.start);
            break;
        }
        case Type::EndQuery:
        {
            auto& d = *reinterpret_cast<const QueryData*>(data);
            list->EndQuery(d.heap, d.type, d.start);
            break;
        }
        case Type::ResolveQueryData:
        {
            auto& d = *reinterpret_cast<const QueryData*>(data);
            list->ResolveQueryData(d.heap, d.type, d.start, d.count, d.destination, d.offset);
            break;
        }
        case Type::Present:
        {
            auto& d = *reinterpret_cast<const PresentData*>(data);
            swapChain->Present(d.syncInterval, d.flags);
            break;
        }
        }
    }

    void DeferredList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
        UINT rectCount, const D3D12_RECT* rects)
    {
        rectCount = std::min<UINT>(rectCount, 4);
        auto& d = Add<ClearDepthData>(Type::ClearDepth, rectCount);
        d = { dsv, flags, depth, stencil };
        std::copy_n(rects, rectCount, d.rects);
        Recorded();
    }

    void DeferredList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rectCount, const D3D12_RECT* rects)
    {
        rectCount = std::min<UINT>(rectCount, 4);
        auto& d = Add<ClearColorData>(Type::ClearColor, rectCount);
        d.rtv = rtv;
        std::copy_n(color, 4, d.color);
        std::copy_n(rects, rectCount, d.rects);
        Recorded();
    }

    void DeferredList::Close()
    {
        Add<uint32_t>(Type::Close);
        Recorded();
    }

    void DeferredList::CopyResource(ID3D12Resource* dest, ID3D12Resource* source)
    {
        Add<CopyResourceData>(Type::CopyResource) = { dest, source };
        Recorded();
    }

    void DeferredList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dest, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box)
    {
        auto& d = Add<CopyTextureData>(Type::CopyTexture);
        d.dest = *dest;
        d.source = *source;
        d.x = x;
        d.y = y;
        d.z = z;
        d.hasBox = box != nullptr;
        if (box)
            d.box = *box;
        Recorded();
    }

    void DeferredList::DrawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance)
    {
        Add<DrawIndexedData>(Type::DrawIndexed) = { indexCount, instanceCount, startIndex, baseVertex, startInstance };
        Recorded();
    }

    void DeferredList::DrawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance)
    {
        Add<DrawData>(Type::Draw) = { vertexCount, instanceCount, startVertex, startInstance };
        Recorded();
    }

    void DeferredList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view)
    {
        Add<D3D12_INDEX_BUFFER_VIEW>(Type::IndexBuffer) = *view;
        Recorded();
    }

    void DeferredList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
    {
        Add<D3D12_PRIMITIVE_TOPOLOGY>(Type::Topology) = topology;
        Recorded();
    }

    void DeferredList::OMSetBlendFactor(const FLOAT factor[4])
    {
        auto& d = Add<std::array<FLOAT, 4>>(Type::BlendFactor);
        std::copy_n(factor, 4, d.data());
        Recorded();
    }

    void DeferredList::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
    {
        auto& d = Add<RenderTargetsData>(Type::RenderTargets, count);
        // A single handle stands for consecutive descriptors; the renderer passes arrays.
        std::copy_n(rtvs, single ? std::min<UINT>(count, 1) : std::min<UINT>(count, 8), d.rtvs);
        d.single = single;
        d.hasDsv = dsv != nullptr;
        if (dsv)
            d.dsv = *dsv;
        Recorded();
    }

    void DeferredList::OMSetStencilRef(UINT reference)
    {
        Add<UINT>(Type::StencilRef) = reference;
        Recorded();
    }

    void DeferredList::RSSetScissorRects(UINT count, const D3D12_RECT* rects)
    {
        count = std::min<UINT>(count, 8);
        std::copy_n(rects, count, Add<RectsData>(Type::Scissors, count).rects);
        Recorded();
    }

    void DeferredList::RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports)
    {
        count = std::min<UINT>(count, 8);
        std::copy_n(viewports, count, Add<ViewportsData>(Type::Viewports, count).viewports);
        Recorded();
    }

    void DeferredList::Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* state)
    {
        Add<ResetData>(Type::Reset) = { allocator, state };
        Recorded();
    }

    void DeferredList::ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        constexpr UINT PER_COMMAND = std::size(BarriersData{}.barriers);
        for (UINT i = 0; i < count; i += PER_COMMAND)
        {
            UINT n = std::min(count - i, PER_COMMAND);
            std::copy_n(barriers + i, n, Add<BarriersData>(Type::Barriers, n).barriers);
            Recorded();
        }
    }

    void DeferredList::SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps)
    {
        count = std::min<UINT>(count, 2);
        std::copy_n(heaps, count, Add<HeapsData>(Type::Heaps, count).heaps);
        Recorded();
    }

    void DeferredList::SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
    {
        Add<RootAddressData>(Type::RootCbv) = { index, address };
        Recorded();
    }

    void DeferredList::SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle)
    {
        Add<RootTableData>(Type::RootTable) = { index, handle };
        Recorded();
    }

    void DeferredList::SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
    {
        Add<RootAddressData>(Type::RootSrv) = { index, address };
        Recorded();
    }

    void DeferredList::SetGraphicsRootSignature(ID3D12RootSignature* signature)
    {
        Add<ID3D12RootSignature*>(Type::RootSignature) = signature;
        Recorded();
    }

    void DeferredList::SetPipelineState(ID3D12PipelineState* state)
    {
        Add<ID3D12PipelineState*>(Type::Pipeline) = state;
        Recorded();
    }

    void DeferredList::Execute(uint64_t fenceValue)
    {
        Add<uint64_t>(Type::Execute) = fenceValue;
        Recorded();
        // Submission points start work on the GPU: hand everything over now.
        if (m_threaded && !m_recording.empty())
        {
            {
                std::lock_guard lock(m_mutex);
                m_batches.push_back(std::move(m_recording));
            }
            m_wake.notify_one();
            m_recording = {};
            m_recording.reserve(BATCH_SIZE);
        }
    }

    void DeferredList::SOSetTargets(const D3D12_STREAM_OUTPUT_BUFFER_VIEW* view)
    {
        auto& d = Add<StreamOutputData>(Type::StreamOutput);
        d.set = view != nullptr;
        if (view)
            d.view = *view;
        Recorded();
    }

    void DeferredList::CopyBufferRegion(ID3D12Resource* dest, UINT64 destOffset, ID3D12Resource* source, UINT64 sourceOffset,
        UINT64 size)
    {
        Add<CopyBufferData>(Type::CopyBuffer) = { dest, destOffset, source, sourceOffset, size };
        Recorded();
    }

    void DeferredList::BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
    {
        Add<QueryData>(Type::BeginQuery) = { heap, type, index, 1, nullptr, 0 };
        Recorded();
    }

    void DeferredList::EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
    {
        Add<QueryData>(Type::EndQuery) = { heap, type, index, 1, nullptr, 0 };
        Recorded();
    }

    void DeferredList::ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* destination,
        UINT64 offset)
    {
        Add<QueryData>(Type::ResolveQueryData) = { heap, type, start, count, destination, offset };
        Recorded();
    }

    void DeferredList::ExecuteOther(ID3D12CommandList* other)
    {
        Add<ID3D12CommandList*>(Type::ExecuteOther) = other;
        Recorded();
    }

    void DeferredList::ResetAllocator(ID3D12CommandAllocator* allocator)
    {
        Add<ID3D12CommandAllocator*>(Type::ResetAllocator) = allocator;
        Recorded();
    }

    void DeferredList::Present(UINT syncInterval, UINT flags)
    {
        Add<PresentData>(Type::Present) = { syncInterval, flags };
        Recorded();
        if (m_threaded && !m_recording.empty())
        {
            {
                std::lock_guard lock(m_mutex);
                m_batches.push_back(std::move(m_recording));
            }
            m_wake.notify_one();
            m_recording = {};
            m_recording.reserve(BATCH_SIZE);
        }
    }
}
