#include "GenerateDepthBuffer.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// GenerateDepthBuffer
//--------------------------------------------------------------------------------------

GenerateRasterDepth::GenerateRasterDepth()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // root constants
    m_rootSig.InitAsConstants(0, 1, 0);

    // frame constants
    m_rootSig.InitAsCBV(1, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        Scene::GlobalResource::FRAME_CONSTANTS_BUFFER);

    RenderPassBase::InitRenderPass("RasterDepth", D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    m_psoLib.CompileComputePSO(0, m_rootSigObj.Get(), COMPILED_CS);
    m_descTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(1);
}

void GenerateRasterDepth::Resize(uint32_t w, uint32_t h)
{
    m_depthBuffer = GpuMemory::GetTexture2D("RasterDepth",
        w,
        h,
        DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_STATE_COMMON,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    Direct3DUtil::CreateTexture2DUAV(m_depthBuffer, m_descTable.CPUHandle(0));
}

void GenerateRasterDepth::Render(ComputeCmdList& computeCmdList)
{
    computeCmdList.PIXBeginEvent("GenerateRasterDepth");

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    const uint32_t w = App::GetRenderer().GetRenderWidth();
    const uint32_t h = App::GetRenderer().GetRenderHeight();

    const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, 8u);
    const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, 8u);

    computeCmdList.ResourceBarrier(m_depthBuffer.Resource(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint32_t descHeapIdx = m_descTable.GPUDescriptorHeapIndex();

    m_rootSig.SetRootConstants(0, 1, &descHeapIdx);
    m_rootSig.End(computeCmdList);

    computeCmdList.SetPipelineState(m_psoLib.GetPSO(0));
    computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

    computeCmdList.ResourceBarrier(m_depthBuffer.Resource(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    computeCmdList.PIXEndEvent();
}