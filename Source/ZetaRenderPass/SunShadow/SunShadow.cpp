#include "SunShadow.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Core::Direct3DUtil;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// SunShadow
//--------------------------------------------------------------------------------------

SunShadow::SunShadow()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // frame constants
    m_rootSig.InitAsCBV(0, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1, NUM_CONSTS, 1);

    // BVH
    m_rootSig.InitAsBufferSRV(2, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH);
}

void SunShadow::Init()
{
    constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;;

    auto samplers = App::GetRenderer().GetStaticSamplers();
    RenderPassBase::InitRenderPass("SunShadow", flags, samplers);

    for(int i = 0; i < (int)SHADER::COUNT; i++)
        m_psoLib.CompileComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);

    m_descTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
    CreateResources();

    m_temporalCB.IsTemporalValid = false;
    m_spatialCB.EdgeStoppingShadowStdScale = DefaultParamVals::EdgeStoppingShadowStdScale;
    m_spatialCB.EdgeStoppingNormalExp = DefaultParamVals::EdgeStoppingNormalExp;
    m_spatialCB.MinFilterVar = 0.0f;

    ParamVariant softShadows;
    softShadows.InitBool("Renderer", "Sun", "SoftShadows",
        fastdelegate::MakeDelegate(this, &SunShadow::DoSoftShadowsCallback), m_doSoftShadows);
    App::AddParam(softShadows);

    ParamVariant denoise;
    denoise.InitBool("Renderer", "Sun", "Denoise",
        fastdelegate::MakeDelegate(this, &SunShadow::DenoiseCallback), m_denoise);
    App::AddParam(denoise);

    ParamVariant minVar;
    minVar.InitFloat("Renderer", "Sun", "MinFilterVariance",
        fastdelegate::MakeDelegate(this, &SunShadow::MinFilterVarianceCallback),
        m_spatialCB.MinFilterVar, 0.0f, 8.0f, 1e-2f);
    App::AddParam(minVar);

    //App::AddShaderReloadHandler("SunShadow_DNSR_Temporal", fastdelegate::MakeDelegate(this, &SunShadow::ReloadDNSRTemporal));
    //App::AddShaderReloadHandler("SunShadow_DNSR_Spatial", fastdelegate::MakeDelegate(this, &SunShadow::ReloadDNSRSpatial));
    App::AddShaderReloadHandler("SunShadow_Trace", fastdelegate::MakeDelegate(this, &SunShadow::ReloadSunShadowTrace));
}

void SunShadow::OnWindowResized()
{
    CreateResources();
    m_temporalCB.IsTemporalValid = false;
    m_currTemporalIdx = 0;
}

void SunShadow::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    // shadow mask
    {
        computeCmdList.PIXBeginEvent("SunShadowTrace");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SunShadowTrace");

        computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

        auto barrier = TextureBarrier_SrvToUavNoSync(m_shadowMask.Resource());
        computeCmdList.ResourceBarrier(barrier);

        cbSunShadow localCB;
        localCB.OutShadowMaskDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::SHADOW_MASK_UAV);
        localCB.SoftShadows = m_doSoftShadows;

        m_rootSig.SetRootConstants(0, sizeof(cbSunShadow) / sizeof(DWORD), &localCB);
        m_rootSig.End(computeCmdList);

        const uint32_t numGroupsX = CeilUnsignedIntDiv(w, SUN_SHADOW_THREAD_GROUP_SIZE_X);
        const uint32_t numGroupsY = CeilUnsignedIntDiv(h, SUN_SHADOW_THREAD_GROUP_SIZE_Y);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::SHADOW_MASK));
        computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }

    computeCmdList.PIXBeginEvent("ShadowDnsr");
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ShadowDnsr");
    
    // temporal pass
    {
        D3D12_TEXTURE_BARRIER barriers[2];
        barriers[0] = TextureBarrier_UavToSrvWithSync(m_shadowMask.Resource());
        barriers[1] = TextureBarrier_SrvToUavNoSync(m_metadata.Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

        int temporalCacheSRV = m_currTemporalIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_0_SRV :
            (int)DESC_TABLE::TEMPORAL_CACHE_1_SRV;
        int temporalCacheUAV = m_currTemporalIdx == 1 ? (int)DESC_TABLE::TEMPORAL_CACHE_1_UAV :
            (int)DESC_TABLE::TEMPORAL_CACHE_0_UAV;

        m_temporalCB.ShadowMaskSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::SHADOW_MASK_SRV);
        m_temporalCB.MomentsUAVHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::MOMENTS_UAV);
        m_temporalCB.MetadataUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::METADATA_UAV);
        m_temporalCB.PrevTemporalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
        m_temporalCB.CurrTemporalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);
        m_temporalCB.NumShadowMaskThreadGroupsX = (uint16_t)CeilUnsignedIntDiv(w, 8u);
        m_temporalCB.NumShadowMaskThreadGroupsY = (uint16_t)CeilUnsignedIntDiv(h, 4u);
        m_temporalCB.DenoisedDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DENOISED_UAV);
        m_temporalCB.Denoise = m_denoise;

        m_rootSig.SetRootConstants(0, sizeof(cbFFX_DNSR_Temporal) / sizeof(DWORD), &m_temporalCB);
        m_rootSig.End(computeCmdList);

        const int numGroupsX = CeilUnsignedIntDiv(w, DNSR_TEMPORAL_THREAD_GROUP_SIZE_X);
        const int numGroupsY = CeilUnsignedIntDiv(h, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::DNSR_TEMPORAL_PASS));
        computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);
    }

    // spatial filter
    {
        const int numGroupsX = CeilUnsignedIntDiv(w, DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_X);
        const int numGroupsY = CeilUnsignedIntDiv(h, DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y);
        m_spatialCB.MetadataSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((uint32_t)DESC_TABLE::METADATA_SRV);

        D3D12_TEXTURE_BARRIER barriers[3];
        barriers[0] = TextureBarrier_UavToSrvWithSync(m_metadata.Resource());
        barriers[1] = TextureBarrier_UavToSrvWithSync(m_temporalCache[m_currTemporalIdx].Resource());
        barriers[2] = m_temporalCB.IsTemporalValid ?
            TextureBarrier_SrvToUavWithSync(m_temporalCache[1 - m_currTemporalIdx].Resource()) :
            TextureBarrier_SrvToUavNoSync(m_temporalCache[1 - m_currTemporalIdx].Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
            
        // ping-pong between temporal 0 & 1
        int temporalCacheSRV = m_currTemporalIdx == 0 ? (int)DESC_TABLE::TEMPORAL_CACHE_0_SRV :
            (int)DESC_TABLE::TEMPORAL_CACHE_1_SRV;
        int temporalCacheUAV = m_currTemporalIdx == 0 ? (int)DESC_TABLE::TEMPORAL_CACHE_1_UAV :
            (int)DESC_TABLE::TEMPORAL_CACHE_0_UAV;

        m_spatialCB.PassNum = 0;
        m_spatialCB.StepSize = 1;
        m_spatialCB.WriteDenoised = m_denoise;
        m_spatialCB.InTemporalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheSRV);
        m_spatialCB.OutTemporalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(temporalCacheUAV);
        m_spatialCB.DenoisedDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DENOISED_UAV);

        m_rootSig.SetRootConstants(0, sizeof(cbFFX_DNSR_Spatial) / sizeof(DWORD), &m_spatialCB);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::DNSR_SPATIAL_FILTER));
        computeCmdList.Dispatch(numGroupsX, numGroupsY, 1);
    }
    
    gpuTimer.EndQuery(computeCmdList, queryIdx);
    computeCmdList.PIXEndEvent();

    m_currTemporalIdx = 1 - m_currTemporalIdx;
    m_temporalCB.IsTemporalValid = true;
}

void SunShadow::CreateResources()
{
    const uint32_t w = App::GetRenderer().GetRenderWidth();
    const uint32_t h = App::GetRenderer().GetRenderHeight();

    // shadow mask
    {
        const uint32_t texWidth = CeilUnsignedIntDiv(w, 8u);
        const uint32_t texHeight = CeilUnsignedIntDiv(h, 4u);

        m_shadowMask = GpuMemory::GetTexture2D("SunShadowMask",
            texWidth, texHeight,
            ResourceFormats::SHADOW_MASK,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DSRV(m_shadowMask, m_descTable.CPUHandle((uint32_t)DESC_TABLE::SHADOW_MASK_SRV));
        Direct3DUtil::CreateTexture2DUAV(m_shadowMask, m_descTable.CPUHandle((uint32_t)DESC_TABLE::SHADOW_MASK_UAV));
    }

    // metadata
    {
        const int texWidth = CeilUnsignedIntDiv(w, DNSR_TEMPORAL_THREAD_GROUP_SIZE_X);
        const int texHeight = CeilUnsignedIntDiv(h, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y);

        m_metadata = GpuMemory::GetTexture2D("SunShadowMetadata",
            texWidth, texHeight,
            ResourceFormats::THREAD_GROUP_METADATA,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DSRV(m_metadata, m_descTable.CPUHandle((uint32_t)DESC_TABLE::METADATA_SRV));
        Direct3DUtil::CreateTexture2DUAV(m_metadata, m_descTable.CPUHandle((uint32_t)DESC_TABLE::METADATA_UAV));
    }

    // moments
    {
        m_moments = GpuMemory::GetTexture2D("SunShadowMoments",
            w, h,
            ResourceFormats::MOMENTS,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DUAV(m_moments, m_descTable.CPUHandle((uint32_t)DESC_TABLE::MOMENTS_UAV));
    }

    // temporal cache
    {
        // start in uav layout to avoid a layout-change barrier
        m_temporalCache[0] = GpuMemory::GetTexture2D("SunShadowTemporal_0",
            w, h,
            ResourceFormats::TEMPORAL_CACHE,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        m_temporalCache[1] = GpuMemory::GetTexture2D("SunShadowTemporal_1",
            w, h,
            ResourceFormats::TEMPORAL_CACHE,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DSRV(m_temporalCache[0], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_0_SRV));
        Direct3DUtil::CreateTexture2DUAV(m_temporalCache[0], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_0_UAV));

        Direct3DUtil::CreateTexture2DSRV(m_temporalCache[1], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_1_SRV));
        Direct3DUtil::CreateTexture2DUAV(m_temporalCache[1], m_descTable.CPUHandle((uint32_t)DESC_TABLE::TEMPORAL_CACHE_1_UAV));
    }

    // denoised
    {
        m_denoised = GpuMemory::GetTexture2D("SunShadowDenoised",
            w, h,
            ResourceFormats::DENOISED,
            D3D12_RESOURCE_STATE_COMMON,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DUAV(m_denoised, m_descTable.CPUHandle((uint32_t)DESC_TABLE::DENOISED_UAV));
    }
}

void SunShadow::DoSoftShadowsCallback(const Support::ParamVariant& p)
{
    m_doSoftShadows = p.GetBool();
}

void SunShadow::DenoiseCallback(const Support::ParamVariant& p)
{
    m_denoise = p.GetBool();
}

//void SunShadow::NumSpatialFilterPassesCallback(const Support::ParamVariant& p)
//{
//    m_numSpatialPasses = p.GetInt().m_val;
//}

void SunShadow::MinFilterVarianceCallback(const Support::ParamVariant& p)
{
    m_spatialCB.MinFilterVar = p.GetFloat().m_value;
}

void SunShadow::EdgeStoppingShadowStdScaleCallback(const Support::ParamVariant& p)
{
    m_spatialCB.EdgeStoppingShadowStdScale = p.GetFloat().m_value;
}

void SunShadow::ReloadDNSRTemporal()
{
    const int i = (int)SHADER::DNSR_TEMPORAL_PASS;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "SunShadow\\ffx_denoiser_temporal.hlsl");
}

void SunShadow::ReloadDNSRSpatial()
{
    const int i = (int)SHADER::DNSR_SPATIAL_FILTER;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "SunShadow\\ffx_denoiser_spatial_filter.hlsl");
}

void SunShadow::ReloadSunShadowTrace()
{
    const int i = (int)SHADER::SHADOW_MASK;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "SunShadow\\SunShadow.hlsl");
}
