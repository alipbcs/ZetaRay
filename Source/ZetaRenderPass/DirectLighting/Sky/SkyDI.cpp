#include "SkyDI.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <Support/Task.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// SkyDI
//--------------------------------------------------------------------------------------

SkyDI::SkyDI()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // root constants
    m_rootSig.InitAsConstants(0,
        NUM_CONSTS,
        1);

    // frame constants
    m_rootSig.InitAsCBV(1,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // BVH
    m_rootSig.InitAsBufferSRV(2,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH);
}

SkyDI::~SkyDI()
{
    Reset();
}

void SkyDI::Init()
{
    constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto& renderer = App::GetRenderer();
    auto samplers = renderer.GetStaticSamplers();
    RenderPassBase::InitRenderPass("SkyDI", flags, samplers);

    TaskSet ts;

    for (int i = 0; i < (int)SHADERS::COUNT; i++)
    {
        StackStr(buff, n, "SkyDI_shader_%d", i);

        ts.EmplaceTask(buff, [i, this]()
            {
                m_psos[i] = m_psoLib.GetComputePSO_MT(i,
                    m_rootSigObj.Get(),
                    COMPILED_CS[i]);
            });
    }

    WaitObject waitObj;
    ts.Sort();
    ts.Finalize(&waitObj);
    App::Submit(ZetaMove(ts));

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
    CreateOutputs();

    memset(&m_cbSpatioTemporal, 0, sizeof(m_cbSpatioTemporal));
    memset(&m_cbDnsrTemporal, 0, sizeof(m_cbDnsrTemporal));
    memset(&m_cbDnsrSpatial, 0, sizeof(m_cbDnsrSpatial));
    m_cbSpatioTemporal.M_max = DefaultParamVals::TemporalM_max;
    m_cbSpatioTemporal.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
    m_cbDnsrTemporal.MaxTsppDiffuse = m_cbDnsrSpatial.MaxTsppDiffuse = DefaultParamVals::DNSRTspp_Diffuse;
    m_cbDnsrTemporal.MaxTsppSpecular = m_cbDnsrSpatial.MaxTsppSpecular = DefaultParamVals::DNSRTspp_Specular;
    m_cbDnsrTemporal.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
    m_cbDnsrSpatial.MinRoughnessResample = DefaultParamVals::MinRoughnessToResample;
    SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE, true);
    SET_CB_FLAG(m_cbDnsrTemporal, CB_SKY_DI_DNSR_TEMPORAL_FLAGS::DENOISE, true);
    SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::DENOISE, true);
    SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_DIFFUSE, true);
    SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_SPECULAR, true);

    ParamVariant doTemporal;
    doTemporal.InitBool("Renderer", "Direct Lighting (Sky)", "Temporal Resample",
        fastdelegate::MakeDelegate(this, &SkyDI::TemporalResamplingCallback), m_temporalResampling);
    App::AddParam(doTemporal);

    ParamVariant doSpatial;
    doSpatial.InitBool("Renderer", "Direct Lighting (Sky)", "Spatial Resample",
        fastdelegate::MakeDelegate(this, &SkyDI::SpatialResamplingCallback), m_spatialResampling);
    App::AddParam(doSpatial);

    ParamVariant maxTemporalM;
    maxTemporalM.InitInt("Renderer", "Direct Lighting (Sky)", "M_max",
        fastdelegate::MakeDelegate(this, &SkyDI::MaxTemporalMCallback),
        m_cbSpatioTemporal.M_max,
        1,
        32,
        1);
    App::AddParam(maxTemporalM);

    //ParamVariant checkerboarding;
    //checkerboarding.InitBool("Renderer", "Direct Lighting (Sky)", "Checkerboarding",
    //    fastdelegate::MakeDelegate(this, &SkyDI::CheckerboardingCallback), IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::CHECKERBOARDING));
    //App::AddParam(checkerboarding);

    ParamVariant minRoughness;
    minRoughness.InitFloat("Renderer", "Direct Lighting (Sky)", "Min Resample Roughness",
        fastdelegate::MakeDelegate(this, &SkyDI::MinRoughnessResampleCallback),
        m_cbSpatioTemporal.MinRoughnessResample,    // val    
        0,                                          // min
        1,                                          // max
        0.1f);                                      // step
    App::AddParam(minRoughness);

    ParamVariant denoise;
    denoise.InitBool("Renderer", "Direct Lighting (Sky)", "Denoise",
        fastdelegate::MakeDelegate(this, &SkyDI::DenoisingCallback), IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE));
    App::AddParam(denoise);

    ParamVariant tsppDiffuse;
    tsppDiffuse.InitInt("Renderer", "Direct Lighting (Sky)", "TSPP (Diffuse)",
        fastdelegate::MakeDelegate(this, &SkyDI::TsppDiffuseCallback),
        m_cbDnsrTemporal.MaxTsppDiffuse,            // val
        1,                                          // min
        32,                                         // max
        1);                                         // step
    App::AddParam(tsppDiffuse);

    ParamVariant tsppSpecular;
    tsppSpecular.InitInt("Renderer", "Direct Lighting (Sky)", "TSPP (Specular)",
        fastdelegate::MakeDelegate(this, &SkyDI::TsppSpecularCallback),
        m_cbDnsrTemporal.MaxTsppSpecular,            // val
        1,                                           // min
        32,                                          // max
        1);                                          // step
    App::AddParam(tsppSpecular);

    ParamVariant dnsrSpatialFilterDiffuse;
    dnsrSpatialFilterDiffuse.InitBool("Renderer", "Direct Lighting (Sky)", "Spatial Filter (Diffuse)",
        fastdelegate::MakeDelegate(this, &SkyDI::DnsrSpatialFilterDiffuseCallback), 
        IS_CB_FLAG_SET(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_DIFFUSE));
    App::AddParam(dnsrSpatialFilterDiffuse);

    ParamVariant dnsrSpatialFilterSpecular;
    dnsrSpatialFilterSpecular.InitBool("Renderer", "Direct Lighting (Sky)", "Spatial Filter (Specular)",
        fastdelegate::MakeDelegate(this, &SkyDI::DnsrSpatialFilterSpecularCallback), 
        IS_CB_FLAG_SET(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_SPECULAR));
    App::AddParam(dnsrSpatialFilterSpecular);

    App::AddShaderReloadHandler("SkyDI", fastdelegate::MakeDelegate(this, &SkyDI::ReloadTemporalPass));
    //App::AddShaderReloadHandler("SkyDI_DNSR_Temporal", fastdelegate::MakeDelegate(this, &SkyDI::ReloadDNSRTemporal));
    //App::AddShaderReloadHandler("SkyDI_DNSR_Spatial", fastdelegate::MakeDelegate(this, &SkyDI::ReloadDNSRSpatial));

    m_isTemporalReservoirValid = false;

    waitObj.Wait();
}

void SkyDI::Reset()
{
    if (IsInitialized())
    {
        App::RemoveShaderReloadHandler("SkyDI_Temporal");
        App::RemoveShaderReloadHandler("SkyDI_DNSR_Temporal");
        App::RemoveShaderReloadHandler("SkyDI_DNSR_Spatial");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Temporal Resample");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Spatial Resample");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "M_max");
        //App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Checkerboarding");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Min Resample Roughness");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Denoise");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "TSPP (Diffuse)");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "TSPP (Specular)");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Spatial Filter (Diffuse)");
        App::RemoveParam("Renderer", "Direct Lighting (Sky)", "Spatial Filter (Specular)");

        for (int i = 0; i < 2; i++)
        {
            m_temporalReservoir[i].ReservoirA.Reset();
            m_temporalReservoir[i].ReservoirB.Reset();
            m_dnsrCache[i].Specular.Reset();
            m_dnsrCache[i].Diffuse.Reset();
        }

        //m_spatialReservoir.ReservoirA.Reset();
        m_denoised.Reset();

        for (int i = 0; i < ZetaArrayLen(m_psos); i++)
            m_psos[i] = nullptr;

        m_descTable.Reset();

        RenderPassBase::ResetRenderPass();
    }
}

void SkyDI::OnWindowResized()
{
    CreateOutputs();
    m_isTemporalReservoirValid = false;
    m_currTemporalIdx = 0;
}

void SkyDI::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    // temporal resampling
    {
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, SKY_DI_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, SKY_DI_TEMPORAL_GROUP_DIM_Y);

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_Temporal");

        computeCmdList.PIXBeginEvent("SkyDI_Temporal");
        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::TEMPORAL_RESAMPLE]);

        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, 6> textureBarriers;

        // transition current temporal reservoir into write state
        textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirA.Resource(),
            D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
            D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
        textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[m_currTemporalIdx].ReservoirB.Resource(),
            D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
            D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));

        // transition color outputs into write state
        if (IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE))
        {
            textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_colorA.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
            textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_colorB.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
        }

        // transition previous reservoirs into read state
        if (m_isTemporalReservoirValid)
        {
            textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirA.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
            textureBarriers.emplace_back(Direct3DUtil::TextureBarrier(m_temporalReservoir[1 - m_currTemporalIdx].ReservoirB.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
        }

        computeCmdList.ResourceBarrier(textureBarriers.data(), (UINT)textureBarriers.size());

        m_cbSpatioTemporal.DispatchDimX = (uint16_t)dispatchDimX;
        m_cbSpatioTemporal.DispatchDimY = (uint16_t)dispatchDimY;
        m_cbSpatioTemporal.NumGroupsInTile = SKY_DI_TEMPORAL_TILE_WIDTH * m_cbSpatioTemporal.DispatchDimY;
        SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::TEMPORAL_RESAMPLE, m_temporalResampling && m_isTemporalReservoirValid);
        SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::SPATIAL_RESAMPLE, m_spatialResampling && m_isTemporalReservoirValid);

        auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_A_SRV : DESC_TABLE::RESERVOIR_1_A_SRV;
        auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_B_SRV : DESC_TABLE::RESERVOIR_1_B_SRV;
        auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_UAV : DESC_TABLE::RESERVOIR_0_A_UAV;
        auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_B_UAV : DESC_TABLE::RESERVOIR_0_B_UAV;

        m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
        m_cbSpatioTemporal.PrevReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
        m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
        m_cbSpatioTemporal.CurrReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);

        m_cbSpatioTemporal.ColorAUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_A_UAV);
        m_cbSpatioTemporal.ColorBUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_UAV);
        m_cbSpatioTemporal.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);;

        m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);
        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        cmdList.PIXEndEvent();
    }

    // denoiser
    if(IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE))
    {
        // denoiser - temporal
        {
            computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_TEMPORAL]);

            // record the timestamp prior to execution
            const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_DNSR_Temporal");

            computeCmdList.PIXBeginEvent("SkyDI_DNSR_Temporal");

            D3D12_TEXTURE_BARRIER barriers[4];

            // transition color into read state
            barriers[0] = Direct3DUtil::TextureBarrier(m_colorA.Resource(),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            barriers[1] = Direct3DUtil::TextureBarrier(m_colorB.Resource(),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            // transition current denoiser caches into write
            barriers[2] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            barriers[3] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Specular.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

            computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

            auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV;
            auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV;
            auto uavDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV;
            auto uavSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV;

            m_cbDnsrTemporal.ColorASrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_A_SRV);
            m_cbDnsrTemporal.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
            m_cbDnsrTemporal.PrevTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
            m_cbDnsrTemporal.PrevTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
            m_cbDnsrTemporal.CurrTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavDiffuseIdx);
            m_cbDnsrTemporal.CurrTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavSpecularIdx);
            SET_CB_FLAG(m_cbDnsrTemporal, CB_SKY_DI_DNSR_TEMPORAL_FLAGS::CACHE_VALID, m_isDnsrTemporalCacheValid);

            m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrTemporal) / sizeof(DWORD), &m_cbDnsrTemporal);
            m_rootSig.End(computeCmdList);

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_Y);
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

            // record the timestamp after execution
            gpuTimer.EndQuery(computeCmdList, queryIdx);

            cmdList.PIXEndEvent();
        }

        // denoiser - spatial
        {
            computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL]);

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, SKY_DI_DNSR_SPATIAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y);

            // record the timestamp prior to execution
            const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_DNSR_Spatial");

            computeCmdList.PIXBeginEvent("SkyDI_DNSR_Spatial");

            D3D12_TEXTURE_BARRIER spatialBarriers[2];

            // transition color into read state
            spatialBarriers[0] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource(),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            spatialBarriers[1] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Specular.Resource(),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            computeCmdList.ResourceBarrier(spatialBarriers, ZetaArrayLen(spatialBarriers));

            auto spatialSrvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV : 
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV;
            auto spatialSrvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV : 
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV;

            m_cbDnsrSpatial.TemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)spatialSrvDiffuseIdx);
            m_cbDnsrSpatial.TemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)spatialSrvSpecularIdx);
            m_cbDnsrSpatial.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
            m_cbDnsrSpatial.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);
            m_cbDnsrSpatial.DispatchDimX = (uint16_t)dispatchDimX;
            m_cbDnsrSpatial.DispatchDimY = (uint16_t)dispatchDimY;
            m_cbDnsrSpatial.NumGroupsInTile = SKY_DI_DNSR_SPATIAL_TILE_WIDTH * m_cbDnsrSpatial.DispatchDimY;

            m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrSpatial) / sizeof(DWORD), &m_cbDnsrSpatial);
            m_rootSig.End(computeCmdList);

            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

            // record the timestamp after execution
            gpuTimer.EndQuery(computeCmdList, queryIdx);

            cmdList.PIXEndEvent();
        }
    }

    m_isTemporalReservoirValid = true;
    m_currTemporalIdx = 1 - m_currTemporalIdx;
    m_isDnsrTemporalCacheValid = IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE);
}

void SkyDI::CreateOutputs()
{
    auto& renderer = App::GetRenderer();

    auto func = [&renderer, this](Texture& tex, DXGI_FORMAT format, const char* name, 
        DESC_TABLE srv, DESC_TABLE uav)
    {
        tex = GpuMemory::GetTexture2D(name,
            renderer.GetRenderWidth(), renderer.GetRenderHeight(),
            format,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DSRV(tex, m_descTable.CPUHandle((int)srv));
        Direct3DUtil::CreateTexture2DUAV(tex, m_descTable.CPUHandle((int)uav));
    };

    // reservoir
    {
        func(m_temporalReservoir[0].ReservoirA, ResourceFormats::RESERVOIR_A, "SkyDI_TemporalReservoir_0_A",
            DESC_TABLE::RESERVOIR_0_A_SRV, DESC_TABLE::RESERVOIR_0_A_UAV);
        func(m_temporalReservoir[0].ReservoirB, ResourceFormats::RESERVOIR_B, "SkyDI_TemporalReservoir_0_B",
            DESC_TABLE::RESERVOIR_0_B_SRV, DESC_TABLE::RESERVOIR_0_B_UAV);
        func(m_temporalReservoir[1].ReservoirA, ResourceFormats::RESERVOIR_A, "SkyDI_TemporalReservoir_1_A",
            DESC_TABLE::RESERVOIR_1_A_SRV, DESC_TABLE::RESERVOIR_1_A_UAV);
        func(m_temporalReservoir[1].ReservoirB, ResourceFormats::RESERVOIR_B, "SkyDI_TemporalReservoir_1_B",
            DESC_TABLE::RESERVOIR_1_B_SRV, DESC_TABLE::RESERVOIR_1_B_UAV);
    }

    {
        func(m_colorA, ResourceFormats::COLOR_A, "RDI_COLOR_A", DESC_TABLE::COLOR_A_SRV, DESC_TABLE::COLOR_A_UAV);
        func(m_colorB, ResourceFormats::COLOR_B, "RDI_COLOR_B", DESC_TABLE::COLOR_B_SRV, DESC_TABLE::COLOR_B_UAV);
    }

    // denoiser cache
    {
        func(m_dnsrCache[0].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Diffuse_0",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV);
        func(m_dnsrCache[1].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Diffuse_1",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV);
        func(m_dnsrCache[0].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Specular_0",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV);
        func(m_dnsrCache[1].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "SkyDI_DNSR_Specular_1",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV);

        m_denoised = GpuMemory::GetTexture2D("SkyDI_Denoised",
            renderer.GetRenderWidth(), renderer.GetRenderHeight(),
            ResourceFormats::DNSR_TEMPORAL_CACHE,
            D3D12_RESOURCE_STATE_COMMON,
            CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DUAV(m_denoised, m_descTable.CPUHandle((int)DESC_TABLE::DNSR_FINAL_UAV));
    }
}

void SkyDI::TemporalResamplingCallback(const Support::ParamVariant& p)
{
    m_temporalResampling = p.GetBool();
}

void SkyDI::SpatialResamplingCallback(const Support::ParamVariant& p)
{
    m_spatialResampling = p.GetBool();
}

void SkyDI::MaxTemporalMCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.M_max = (uint16_t)p.GetInt().m_value;
}

void SkyDI::MinRoughnessResampleCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.MinRoughnessResample = p.GetFloat().m_value;
    m_cbDnsrTemporal.MinRoughnessResample = p.GetFloat().m_value;
    m_cbDnsrSpatial.MinRoughnessResample = p.GetFloat().m_value;
}

void SkyDI::DenoisingCallback(const Support::ParamVariant& p)
{
    m_isDnsrTemporalCacheValid = !IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE) ?
        false : m_isDnsrTemporalCacheValid;
    SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE, p.GetBool());
    SET_CB_FLAG(m_cbDnsrTemporal, CB_SKY_DI_DNSR_TEMPORAL_FLAGS::DENOISE, p.GetBool());
    SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::DENOISE, p.GetBool());
    m_isDnsrTemporalCacheValid = !IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE) ?
        false : m_isDnsrTemporalCacheValid;
}

void SkyDI::TsppDiffuseCallback(const Support::ParamVariant& p)
{
    m_cbDnsrTemporal.MaxTsppDiffuse = (uint16_t)p.GetInt().m_value;
}

void SkyDI::TsppSpecularCallback(const Support::ParamVariant& p)
{
    m_cbDnsrTemporal.MaxTsppSpecular = (uint16_t)p.GetInt().m_value;
}

void SkyDI::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_DIFFUSE, p.GetBool());
}

void SkyDI::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_SPECULAR, p.GetBool());
}

//void SkyDI::CheckerboardingCallback(const Support::ParamVariant& p)
//{
//    SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::CHECKERBOARDING, p.GetBool());
//}

void SkyDI::ReloadTemporalPass()
{
    const int i = (int)SHADERS::TEMPORAL_RESAMPLE;

    m_psoLib.Reload(i, "DirectLighting\\Sky\\SkyDI_SpatioTemporal.hlsl", true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void SkyDI::ReloadDNSRTemporal()
{
    const int i = (int)SHADERS::DNSR_TEMPORAL;

    m_psoLib.Reload(i, "DirectLighting\\Sky\\SkyDI_DNSR_Temporal.hlsl", true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void SkyDI::ReloadDNSRSpatial()
{
    const int i = (int)SHADERS::DNSR_SPATIAL;

    m_psoLib.Reload(i, "DirectLighting\\Sky\\SkyDI_DNSR_Spatial.hlsl", true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}
