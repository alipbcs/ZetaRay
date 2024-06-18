#pragma once

#include "Direct3DUtil.h"
#include "RootSignature.h"
#include "../Utility/Error.h"
#define USE_PIX
#include <WinPixEventRuntime/pix3.h>

namespace ZetaRay::Core
{
    //--------------------------------------------------------------------------------------
    // CommandList
    //--------------------------------------------------------------------------------------

    class CommandList
    {
        friend struct CommandQueue;

    public:
        ~CommandList() = default;

        CommandList(const CommandList&) = delete;
        CommandList& operator=(const CommandList&) = delete;

        void Reset(ID3D12CommandAllocator* cmdAlloc);
        ZetaInline D3D12_COMMAND_LIST_TYPE GetType() const { return m_type; }

        ZetaInline void PIXBeginEvent(const char* s)
        {
            ::PIXBeginEvent(m_cmdList.Get(), PIX_COLOR_DEFAULT, s);
        }

        ZetaInline void PIXEndEvent()
        {
            ::PIXEndEvent(m_cmdList.Get());
        }

        ZetaInline void SetName(const char* s)
        {
            m_cmdList->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(s), s);
        }

        ZetaInline ID3D12GraphicsCommandList7* Get()
        {
            return m_cmdList.Get();
        }

    protected:
        CommandList(D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* cmdAlloc);

        D3D12_COMMAND_LIST_TYPE m_type;
        ComPtr<ID3D12GraphicsCommandList7> m_cmdList;
        ID3D12CommandAllocator* m_cmdAllocator = nullptr;
    };

    //--------------------------------------------------------------------------------------
    // CopyContext
    //--------------------------------------------------------------------------------------

    class CopyCmdList : public CommandList
    {
    public:
        CopyCmdList(D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* cmdAlloc)
            : CommandList(t, cmdAlloc)
        {}

        ZetaInline void ResourceBarrier(ID3D12Resource* res, D3D12_RESOURCE_STATES oldState, D3D12_RESOURCE_STATES newState,
            UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
        {
            Assert(oldState != newState, "Invalid barrier states");
            auto barrier = Direct3DUtil::TransitionBarrier(res, oldState, newState, subresource);
            m_cmdList->ResourceBarrier(1, &barrier);
        }

        ZetaInline void ResourceBarrier(D3D12_RESOURCE_BARRIER* barriers, UINT numBarriers)
        {
            m_cmdList->ResourceBarrier(numBarriers, barriers);
        }

        ZetaInline void ResourceBarrier(D3D12_BUFFER_BARRIER& barrier)
        {
            const auto barrierGroup = Direct3DUtil::BarrierGroup(&barrier, 1);
            m_cmdList->Barrier(1, &barrierGroup);
        }

        ZetaInline void ResourceBarrier(D3D12_BUFFER_BARRIER* barriers, UINT numBarriers)
        {
            const auto barrierGroup = Direct3DUtil::BarrierGroup(barriers, numBarriers);
            m_cmdList->Barrier(1, &barrierGroup);
        }

        ZetaInline void ResourceBarrier(D3D12_TEXTURE_BARRIER& barrier)
        {
            const auto barrierGroup = Direct3DUtil::BarrierGroup(&barrier, 1);
            m_cmdList->Barrier(1, &barrierGroup);
        }

        ZetaInline void ResourceBarrier(D3D12_TEXTURE_BARRIER* barriers, UINT numBarriers)
        {
            const auto barrierGroup = Direct3DUtil::BarrierGroup(barriers, numBarriers);
            m_cmdList->Barrier(1, &barrierGroup);
        }

        ZetaInline void ResourceBarrier(D3D12_BARRIER_GROUP* barrierGroups, UINT numBarrierGroups)
        {
            m_cmdList->Barrier(numBarrierGroups, barrierGroups);
        }

        ZetaInline void UAVBarrier(ID3D12Resource* res)
        {
            D3D12_RESOURCE_BARRIER barriers[1];
            barriers[0] = Direct3DUtil::UAVBarrier(res);
            m_cmdList->ResourceBarrier(1, barriers);
        }

        ZetaInline void UAVBarrier(UINT numBarriers, D3D12_RESOURCE_BARRIER* barriers)
        {
            m_cmdList->ResourceBarrier(numBarriers, barriers);
        }

        ZetaInline void CopyResource(ID3D12Resource* dstResource, ID3D12Resource* srcResource)
        {
            m_cmdList->CopyResource(dstResource, srcResource);
        }

        ZetaInline void CopyBufferRegion(ID3D12Resource* dstBuffer, UINT64 dstOffset, ID3D12Resource* srcBuffer,
            UINT64 srcOffset, UINT64 numBytes)
        {
            m_cmdList->CopyBufferRegion(dstBuffer, dstOffset, srcBuffer, srcOffset, numBytes);
        }

        ZetaInline void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst,
            UINT dstX,
            UINT dstY,
            UINT dstZ,
            const D3D12_TEXTURE_COPY_LOCATION* src,
            const D3D12_BOX* srcBox)
        {
            m_cmdList->CopyTextureRegion(dst, dstX, dstY, dstZ, src, srcBox);
        }
    };

    //--------------------------------------------------------------------------------------
    // ComputeContext
    //--------------------------------------------------------------------------------------

    class ComputeCmdList : public CopyCmdList
    {
    public:
        ComputeCmdList(D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* cmdAlloc)
            : CopyCmdList(t, cmdAlloc)
        {}

        ZetaInline void BeginQuery(ID3D12QueryHeap* queryHeap, D3D12_QUERY_TYPE type, UINT index)
        {
            m_cmdList->BeginQuery(queryHeap, type, index);
        }

        ZetaInline void ResolveQueryData(ID3D12QueryHeap* queryHeap,
            D3D12_QUERY_TYPE type,
            UINT startIndex,
            UINT numQueries,
            ID3D12Resource* destinationBuffer,
            UINT64 alignedDestinationBufferOffset)
        {
            m_cmdList->ResolveQueryData(queryHeap, type, startIndex, numQueries, destinationBuffer, alignedDestinationBufferOffset);
        }

        ZetaInline void EndQuery(ID3D12QueryHeap* queryHeap, D3D12_QUERY_TYPE type, UINT index)
        {
            m_cmdList->EndQuery(queryHeap, type, index);
        }

        ZetaInline void ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE viewGPUHandleInCurrentHeap,
            D3D12_CPU_DESCRIPTOR_HANDLE viewCPUHandle,
            ID3D12Resource* resource,
            float clearX = 0.0f, float clearY = 0.0f, float clearZ = 0.0f, float ClearW = 0.0f,
            UINT numRects = 0,
            const D3D12_RECT* rects = nullptr)
        {
            FLOAT values[4] = { clearX, clearY, clearZ, ClearW };

            m_cmdList->ClearUnorderedAccessViewFloat(
                viewGPUHandleInCurrentHeap,
                viewCPUHandle,
                resource,
                values,
                numRects,
                rects);
        }

        ZetaInline void ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE viewGPUHandleInCurrentHeap,
            D3D12_CPU_DESCRIPTOR_HANDLE viewCPUHandle,
            ID3D12Resource* resource,
            UINT clearX = 0, UINT clearY = 0, UINT clearZ = 0, UINT clearW = 0,
            UINT numRects = 0,
            const D3D12_RECT* rects = nullptr)
        {
            UINT values[4] = { clearX, clearY, clearZ, clearW };

            m_cmdList->ClearUnorderedAccessViewUint(viewGPUHandleInCurrentHeap,
                viewCPUHandle,
                resource,
                values,
                numRects,
                rects);
        }

        ZetaInline void SetRootSignature(RootSignature& rootSig, ID3D12RootSignature* rootSigObj)
        {
            rootSig.Begin();
            Assert(rootSigObj, "rootSigObj was NULL");
            m_cmdList->SetComputeRootSignature(rootSigObj);
        }

        ZetaInline void SetRootSignature(ID3D12RootSignature* rootSigObj)
        {
            Assert(rootSigObj, "rootSigObj was NULL");
            m_cmdList->SetComputeRootSignature(rootSigObj);
        }

        ZetaInline void SetRootConstantBufferView(UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
        {
            m_cmdList->SetComputeRootConstantBufferView(rootParameterIndex, bufferLocation);
        }

        ZetaInline void SetRootShaderResourceView(UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
        {
            m_cmdList->SetComputeRootShaderResourceView(rootParameterIndex, bufferLocation);
        }

        ZetaInline void SetRootDescriptorTable(UINT rootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
        {
            m_cmdList->SetComputeRootDescriptorTable(rootParameterIndex, baseDescriptor);
        }

        ZetaInline void SetRootUnorderedAccessView(UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
        {
            m_cmdList->SetComputeRootUnorderedAccessView(rootParameterIndex, bufferLocation);
        }

        ZetaInline void SetRoot32BitConstants(UINT rootParameterIndex,
            UINT num32BitValuesToSet,
            const void* srcData,
            UINT destOffsetIn32BitValues)
        {
            m_cmdList->SetComputeRoot32BitConstants(rootParameterIndex, num32BitValuesToSet, srcData, destOffsetIn32BitValues);
        }

        ZetaInline void SetPipelineState(ID3D12PipelineState* pipelineState)
        {
            Assert(pipelineState, "pipelineState was NULL");
            m_cmdList->SetPipelineState(pipelineState);
        }

        ZetaInline void SetPipelineState1(ID3D12StateObject* rtPSO)
        {
            Assert(rtPSO, "rtPSO was NULL");
            m_cmdList->SetPipelineState1(rtPSO);
        }

        ZetaInline void Dispatch(UINT threadGroupCountX, UINT threadGroupCountY, UINT threadGroupCountZ)
        {
            m_cmdList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
        }

        ZetaInline void SetPredication(ID3D12Resource* buffer, UINT64 bufferOffset, D3D12_PREDICATION_OP op)
        {
            m_cmdList->SetPredication(buffer, bufferOffset, op);
        }

        ZetaInline void BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc,
            UINT numPostbuildInfoDescs,
            const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* postbuildInfoDescs)
        {
            m_cmdList->BuildRaytracingAccelerationStructure(desc, numPostbuildInfoDescs, postbuildInfoDescs);
        }

        ZetaInline void CompactAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS src)
        {
            m_cmdList->CopyRaytracingAccelerationStructure(dest, src, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
        }

        ZetaInline void ExecuteIndirect(ID3D12CommandSignature* cmdSig,
            UINT maxCmdCount,
            ID3D12Resource* argBuffer,
            UINT64 argBufferOffset,
            ID3D12Resource* countBuffer,
            UINT64 countBufferOffset)
        {
            m_cmdList->ExecuteIndirect(cmdSig, maxCmdCount, argBuffer, argBufferOffset, countBuffer, countBufferOffset);
        }

        ZetaInline void DispatchRays(D3D12_GPU_VIRTUAL_ADDRESS rayGenAddr, UINT64 rayGenSizeInBytes,
            D3D12_GPU_VIRTUAL_ADDRESS missTableAddr, UINT64 missTableSizeInBytes, UINT64 missTableStrideInBytes,
            D3D12_GPU_VIRTUAL_ADDRESS hitTableAddr, UINT64 hitTableSizeInBytes, UINT64 hitTableStrideInBytes,
            UINT width,
            UINT height,
            UINT depth = 1)
        {
            D3D12_DISPATCH_RAYS_DESC desc;
            desc.RayGenerationShaderRecord.StartAddress = rayGenAddr;
            desc.RayGenerationShaderRecord.SizeInBytes = rayGenSizeInBytes;
            desc.MissShaderTable.StartAddress = missTableAddr;
            desc.MissShaderTable.SizeInBytes = missTableSizeInBytes;
            desc.MissShaderTable.StrideInBytes = missTableStrideInBytes;
            desc.HitGroupTable.StartAddress = hitTableAddr;
            desc.HitGroupTable.SizeInBytes = hitTableSizeInBytes;
            desc.HitGroupTable.StrideInBytes = hitTableStrideInBytes;
            desc.CallableShaderTable.StartAddress = 0;
            desc.CallableShaderTable.SizeInBytes = 0;
            desc.CallableShaderTable.StrideInBytes = 0;
            desc.Width = width;
            desc.Height = height;
            desc.Depth = depth;

            m_cmdList->DispatchRays(&desc);
        }
    };

    //--------------------------------------------------------------------------------------
    // GraphicsContext
    //--------------------------------------------------------------------------------------

    class GraphicsCmdList final : public ComputeCmdList
    {
    public:
        explicit GraphicsCmdList(ID3D12CommandAllocator* cmdAlloc)
            : ComputeCmdList(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc)
        {}

        ZetaInline void SetRootSignature(RootSignature& rootSig, ID3D12RootSignature* rootSigObj)
        {
            rootSig.Begin();
            Assert(rootSigObj, "rootSigObj was NULL");
            m_cmdList->SetGraphicsRootSignature(rootSigObj);
        }

        ZetaInline void SetRootConstantBufferView(UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
        {
            m_cmdList->SetGraphicsRootConstantBufferView(rootParameterIndex, bufferLocation);
        }

        ZetaInline void SetRootShaderResourceView(UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
        {
            m_cmdList->SetGraphicsRootShaderResourceView(rootParameterIndex, bufferLocation);
        }

        ZetaInline void SetRootDescriptorTable(UINT rootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
        {
            m_cmdList->SetGraphicsRootDescriptorTable(rootParameterIndex, baseDescriptor);
        }

        ZetaInline void SetRootUnorderedAccessView(UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
        {
            m_cmdList->SetGraphicsRootUnorderedAccessView(rootParameterIndex, bufferLocation);
        }

        ZetaInline void SetRoot32BitConstants(UINT rootParameterIndex,
            UINT num32BitValuesToSet,
            const void* srcData,
            UINT destOffsetIn32BitValues)
        {
            m_cmdList->SetGraphicsRoot32BitConstants(rootParameterIndex, num32BitValuesToSet, srcData, destOffsetIn32BitValues);
        }

        void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView,
            D3D12_CLEAR_FLAGS clearFlags,
            FLOAT depth = 1.0f, UINT8 stencil = 0,
            UINT numRects = 0, const D3D12_RECT* rects = nullptr)
        {
            m_cmdList->ClearDepthStencilView(depthStencilView, clearFlags, depth, stencil, numRects, rects);
        }

        ZetaInline void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView,
            float r, float g, float b, float a,
            UINT numRects = 0, const D3D12_RECT* rects = nullptr)
        {
            FLOAT rgb[4] = { r, g, b, a };
            m_cmdList->ClearRenderTargetView(renderTargetView, rgb, numRects, rects);
        }

        ZetaInline void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitiveTopology)
        {
            m_cmdList->IASetPrimitiveTopology(primitiveTopology);
        }

        ZetaInline void IASetVertexAndIndexBuffers(const D3D12_VERTEX_BUFFER_VIEW& vbv,
            const D3D12_INDEX_BUFFER_VIEW& ibv,
            UINT startSlot = 0)
        {
            m_cmdList->IASetVertexBuffers(startSlot, 1, &vbv);
            m_cmdList->IASetIndexBuffer(&ibv);
        }

        ZetaInline void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& ibv)
        {
            m_cmdList->IASetIndexBuffer(&ibv);
        }

        ZetaInline void DrawInstanced(UINT vertexCountPerInstance,
            UINT instanceCount,
            UINT startVertexLocation,
            UINT startInstanceLocation)
        {
            m_cmdList->DrawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
        }

        ZetaInline void DrawIndexedInstanced(UINT indexCountPerInstance,
            UINT instanceCount,
            UINT startIndexLocation,
            INT  baseVertexLocation,
            UINT startInstanceLocation)
        {
            m_cmdList->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
        }

        ZetaInline void OMSetRenderTargets(UINT numRenderTargetDescriptors, 
            const D3D12_CPU_DESCRIPTOR_HANDLE* renderTargetDescriptors,
            BOOL RTsSingleHandleToDescriptorRange, 
            const D3D12_CPU_DESCRIPTOR_HANDLE* depthStencilDescriptor)
        {
            m_cmdList->OMSetRenderTargets(numRenderTargetDescriptors, renderTargetDescriptors, RTsSingleHandleToDescriptorRange, depthStencilDescriptor);
        }

        ZetaInline void RSSetViewports(UINT num, const D3D12_VIEWPORT* viewports)
        {
            m_cmdList->RSSetViewports(num, viewports);
        }

        ZetaInline void RSSetViewportsScissorsRects(int num, const D3D12_VIEWPORT* viewports, const D3D12_RECT* rects = nullptr)
        {
            m_cmdList->RSSetViewports(num, viewports);

            if (rects)
                m_cmdList->RSSetScissorRects(num, rects);
        }

        ZetaInline void RSSetScissorRects(UINT numRects, const D3D12_RECT* rects)
        {
            m_cmdList->RSSetScissorRects(numRects, rects);
        }

        ZetaInline void OMSetBlendFactor(float blendFactorR, float blendFactorG, float blendFactorB, float blendFactorW)
        {
            FLOAT rgb[4] = { blendFactorR, blendFactorG, blendFactorB, blendFactorW };
            m_cmdList->OMSetBlendFactor(rgb);
        }

        ZetaInline void SetPredication(ID3D12Resource* buffer, UINT64 bufferOffset, D3D12_PREDICATION_OP op)
        {
            m_cmdList->SetPredication(buffer, bufferOffset, op);
        }
    };
}

