#include "AutoExposure.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DUtil;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// AutoExposure
//--------------------------------------------------------------------------------------

AutoExposure::AutoExposure()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // frame constants
    m_rootSig.InitAsCBV(0,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1,
        NUM_CONSTS,
        1);

    m_rootSig.InitAsBufferUAV(2,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
}

AutoExposure::~AutoExposure()
{
    Reset();
}

void AutoExposure::Init()
{
    D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    RenderPassBase::InitRenderPass("AutoExposure", flags);
    m_descTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);

    m_minLum = DefaultParamVals::MinLum;
    m_maxLum = DefaultParamVals::MaxLum;
    m_cbHist.LumMapExp = DefaultParamVals::LumMapExp;
    m_cbHist.AdaptationRate = DefaultParamVals::AdaptationRate;
    m_cbHist.LowerPercentile = DefaultParamVals::LowerPercentile;
    m_cbHist.UpperPercentile = DefaultParamVals::UpperPercentile;

    ParamVariant p1;
    p1.InitFloat("Renderer", "Auto Exposure", "Min Lum", fastdelegate::MakeDelegate(this, &AutoExposure::MinLumCallback),
        DefaultParamVals::MinLum, 1e-4f, 0.5f, 1e-3f);
    App::AddParam(p1);

    ParamVariant p2;
    p2.InitFloat("Renderer", "Auto Exposure", "Max Lum", fastdelegate::MakeDelegate(this, &AutoExposure::MaxLumCallback),
        DefaultParamVals::MaxLum, 0.5f, 8.0f, 1e-2f);
    App::AddParam(p2);

    ParamVariant p5;
    p5.InitFloat("Renderer", "Auto Exposure", "Lower Percentile", fastdelegate::MakeDelegate(this, &AutoExposure::LowerPercentileCallback),
        DefaultParamVals::LowerPercentile, 0.0f, 0.3f, 0.01f);
    App::AddParam(p5);

    ParamVariant p6;
    p6.InitFloat("Renderer", "Auto Exposure", "Upper Percentile", fastdelegate::MakeDelegate(this, &AutoExposure::UpperPercentileCallback),
        DefaultParamVals::UpperPercentile, 0.6f, 1.0f, 0.01f);
    App::AddParam(p6);

    ParamVariant p3;
    p3.InitFloat("Renderer", "Auto Exposure", "Lum Map Exp", fastdelegate::MakeDelegate(this, &AutoExposure::LumMapExpCallback),
        DefaultParamVals::LumMapExp, 1e-1f, 1.0f, 1e-2f);
    App::AddParam(p3);

    m_psos[(int)SHADERS::HISTOGRAM] = m_psoLib.GetComputePSO((int)SHADERS::HISTOGRAM, m_rootSigObj.Get(),
        COMPILED_CS[(int)SHADERS::HISTOGRAM]);
    m_psos[(int)SHADERS::EXPECTED_VALUE] = m_psoLib.GetComputePSO((int)SHADERS::EXPECTED_VALUE, m_rootSigObj.Get(),
        COMPILED_CS[(int)SHADERS::EXPECTED_VALUE]);

    CreateResources();
}

void AutoExposure::Reset()
{
    if (m_psos[0] || m_psos[1])
    {
        m_exposure.Reset();
        m_counter.Reset();
        m_hist.Reset();
        RenderPassBase::ResetRenderPass();
    }
}

void AutoExposure::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    Assert(m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED] != -1, "Input descriptor hasn't been set.");

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.PIXBeginEvent("AutoExposure");
    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    // record the timestamp prior to execution
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "AutoExposure");

    m_cbHist.MinLum = m_minLum;
    m_cbHist.LumRange = m_maxLum - m_minLum;
    m_cbHist.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::COMPOSITED];
    m_cbHist.ExposureDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::EXPOSURE_UAV);

    m_rootSig.SetRootUAV(2, m_hist.GpuVA());
    m_rootSig.SetRootConstants(0, sizeof(cbAutoExposureHist) / sizeof(DWORD), &m_cbHist);
    m_rootSig.End(computeCmdList);

    const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, THREAD_GROUP_SIZE_HIST_X);
    const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, THREAD_GROUP_SIZE_HIST_Y);

    computeCmdList.ResourceBarrier(m_hist.Resource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    computeCmdList.CopyBufferRegion(m_hist.Resource(), 0, m_zeroBuffer.Resource(), 0, HIST_BIN_COUNT * sizeof(uint32_t));
    computeCmdList.ResourceBarrier(m_hist.Resource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    computeCmdList.SetPipelineState(m_psos[(int)SHADERS::HISTOGRAM]);
    computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

    auto uavBarrier = Direct3DUtil::UAVBarrier(m_hist.Resource());
    computeCmdList.UAVBarrier(1, &uavBarrier);

    computeCmdList.SetPipelineState(m_psos[(int)SHADERS::EXPECTED_VALUE]);
    computeCmdList.Dispatch(1, 1, 1);

    computeCmdList.PIXEndEvent();

    // record the timestamp after execution
    gpuTimer.EndQuery(computeCmdList, queryIdx);
}

void AutoExposure::CreateResources()
{
    auto& renderer = App::GetRenderer();

    m_hist = GpuMemory::GetDefaultHeapBuffer("LogLumHistogram",
        HIST_BIN_COUNT * sizeof(uint32_t),
        D3D12_RESOURCE_STATE_COMMON,
        true);

    m_exposure = GpuMemory::GetTexture2D("Exposure",
        1,
        1,
        DXGI_FORMAT_R32G32_FLOAT,
        D3D12_RESOURCE_STATE_COMMON,
        CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS | CREATE_TEXTURE_FLAGS::INIT_TO_ZERO);

    // create a zero-initialized buffer for resetting the counter
    m_zeroBuffer = GpuMemory::GetDefaultHeapBuffer("Zero",
        HIST_BIN_COUNT * sizeof(uint32_t),
        D3D12_RESOURCE_STATE_COMMON,
        false,
        true);

    Direct3DUtil::CreateTexture2DUAV(m_exposure, m_descTable.CPUHandle((int)DESC_TABLE::EXPOSURE_UAV));
}

void AutoExposure::MinLumCallback(const Support::ParamVariant& p)
{
    m_minLum = Math::Min(p.GetFloat().m_value, m_maxLum);
}

void AutoExposure::MaxLumCallback(const Support::ParamVariant& p)
{
    m_maxLum = Math::Max(p.GetFloat().m_value, m_minLum);
}

void AutoExposure::LumMapExpCallback(const Support::ParamVariant& p)
{
    m_cbHist.LumMapExp = p.GetFloat().m_value;
}

void AutoExposure::LowerPercentileCallback(const Support::ParamVariant& p)
{
    m_cbHist.LowerPercentile = p.GetFloat().m_value;
}

void AutoExposure::UpperPercentileCallback(const Support::ParamVariant& p)
{
    m_cbHist.UpperPercentile = p.GetFloat().m_value;
}
