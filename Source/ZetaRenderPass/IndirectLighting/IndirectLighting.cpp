#include "IndirectLighting.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <Scene/SceneCore.h>
#include <Support/Task.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Core::Direct3DUtil;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;

namespace
{
    constexpr int EnumToSamplerIdx(TEXTURE_FILTER f)
    {
        if (f == TEXTURE_FILTER::MIP0)
            return 0;
        if (f == TEXTURE_FILTER::TRI_LINEAR)
            return 4;
        if (f == TEXTURE_FILTER::ANISOTROPIC_4X)
            return 6;

        return 5;
    }
}

//--------------------------------------------------------------------------------------
// IndirectLighting
//--------------------------------------------------------------------------------------

IndirectLighting::IndirectLighting()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // frame constants
    m_rootSig.InitAsCBV(0, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1, NUM_CONSTS, 1, 0);

    // BVH
    m_rootSig.InitAsBufferSRV(2, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH);

    // mesh buffer
    m_rootSig.InitAsBufferSRV(3, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::RT_FRAME_MESH_INSTANCES);

    // scene VB
    m_rootSig.InitAsBufferSRV(4, 2, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::SCENE_VERTEX_BUFFER);

    // scene IB
    m_rootSig.InitAsBufferSRV(5, 3, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::SCENE_INDEX_BUFFER);

    // material buffer
    m_rootSig.InitAsBufferSRV(6, 4, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::MATERIAL_BUFFER);

    // emissive triangles
    m_rootSig.InitAsBufferSRV(7, 5, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::EMISSIVE_TRIANGLE_BUFFER,
        true);

    // sample sets
    m_rootSig.InitAsBufferSRV(8, 6, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::PRESAMPLED_EMISSIVE_SETS,
        true);

    // alias table
    m_rootSig.InitAsBufferSRV(9, 7, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE,
        true);

    // light voxel grid
    m_rootSig.InitAsBufferSRV(10, 8, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::LIGHT_VOXEL_GRID,
        true);
}

void IndirectLighting::Init(INTEGRATOR method)
{
    constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto samplers = App::GetRenderer().GetStaticSamplers();
    RenderPassBase::InitRenderPass("IndirectLighting", flags, samplers);

    constexpr int NumTaskSets = CeilUnsignedIntDiv((int)SHADER::COUNT,
        TaskSet::MAX_NUM_TASKS);
    TaskSet ts[NumTaskSets];
    int i = 0;

    for (int t = 0; t < NumTaskSets; t++)
    {
        for (; i < Min(TaskSet::MAX_NUM_TASKS * (t + 1), (int)SHADER::COUNT); i++)
        {
            StackStr(buff, n, "IndirectShader_%d", i);

            ts[t].EmplaceTask(buff, [i, this]()
                {
                    m_psoLib.CompileComputePSO_MT(i, m_rootSigObj.Get(),
                        COMPILED_CS[i]);
                });
        }

        ts[t].Sort();
        ts[t].Finalize();
        App::Submit(ZetaMove(ts[t]));
    }

    memset(&m_cbRGI, 0, sizeof(m_cbRGI));
    memset(&m_cbRPT_PathTrace, 0, sizeof(m_cbRPT_PathTrace));
    memset(&m_cbRPT_Temporal, 0, sizeof(m_cbRPT_Temporal));
    memset(&m_cbDnsrTemporal, 0, sizeof(m_cbDnsrTemporal));
    memset(&m_cbDnsrSpatial, 0, sizeof(m_cbDnsrSpatial));
    m_cbRGI.M_max = DefaultParamVals::M_MAX;
    m_cbRPT_PathTrace.Alpha_min = m_cbRPT_Temporal.Alpha_min = 
        DefaultParamVals::ROUGHNESS_MIN * DefaultParamVals::ROUGHNESS_MIN;
    m_cbRGI.MaxDiffuseBounces = DefaultParamVals::MAX_DIFFUSE_BOUNCES;
    m_cbRGI.MaxGlossyBounces_NonTr = DefaultParamVals::MAX_GLOSSY_BOUNCES_NON_TRANSMISSIVE;
    m_cbRGI.MaxGlossyBounces_Tr = DefaultParamVals::MAX_GLOSSY_BOUNCES_TRANSMISSIVE;
    m_cbRPT_PathTrace.Packed = m_cbRPT_Temporal.Packed = DefaultParamVals::MAX_DIFFUSE_BOUNCES |
        (DefaultParamVals::MAX_GLOSSY_BOUNCES_NON_TRANSMISSIVE << 4) |
        (DefaultParamVals::MAX_GLOSSY_BOUNCES_TRANSMISSIVE << 8);
    m_cbRPT_PathTrace.Packed |= (DefaultParamVals::M_MAX << 16);
    m_cbRPT_PathTrace.TexFilterDescHeapIdx = EnumToSamplerIdx(DefaultParamVals::TEX_FILTER);
    m_cbRGI.TexFilterDescHeapIdx = m_cbRPT_Temporal.TexFilterDescHeapIdx = m_cbRPT_PathTrace.TexFilterDescHeapIdx;
    m_cbRPT_Temporal.Packed |= (DefaultParamVals::M_MAX << 16);
    m_cbRPT_Temporal.MaxSpatialM = DefaultParamVals::M_MAX_SPATIAL;
    m_cbDnsrTemporal.MaxTsppDiffuse = m_cbDnsrSpatial.MaxTsppDiffuse = DefaultParamVals::DNSR_TSPP_DIFFUSE;
    m_cbDnsrTemporal.MaxTsppSpecular = m_cbDnsrSpatial.MaxTsppSpecular = DefaultParamVals::DNSR_TSPP_SPECULAR;

    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::RUSSIAN_ROULETTE, DefaultParamVals::RUSSIAN_ROULETTE);
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::RUSSIAN_ROULETTE, DefaultParamVals::RUSSIAN_ROULETTE);
    SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::RUSSIAN_ROULETTE, DefaultParamVals::RUSSIAN_ROULETTE);
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::SORT_TEMPORAL, true);
    SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_TEMPORAL, true);

    ParamVariant rr;
    rr.InitBool("Renderer", "Indirect Lighting", "Russian Roulette",
        fastdelegate::MakeDelegate(this, &IndirectLighting::RussianRouletteCallback),
        DefaultParamVals::RUSSIAN_ROULETTE, "Path Sampling");
    App::AddParam(rr);

    ParamVariant maxDiffuseBounces;
    maxDiffuseBounces.InitInt("Renderer", "Indirect Lighting", "Max Diffuse Bounces",
        fastdelegate::MakeDelegate(this, &IndirectLighting::MaxDiffuseBouncesCallback),
        DefaultParamVals::MAX_DIFFUSE_BOUNCES, 1, 8, 1, "Path Sampling");
    App::AddParam(maxDiffuseBounces);

    ParamVariant maxGlossyBounces;
    maxGlossyBounces.InitInt("Renderer", "Indirect Lighting", "Max Glossy Bounces",
        fastdelegate::MakeDelegate(this, &IndirectLighting::MaxGlossyBouncesCallback),
        DefaultParamVals::MAX_GLOSSY_BOUNCES_NON_TRANSMISSIVE, 1, 8, 1, "Path Sampling");
    App::AddParam(maxGlossyBounces);

    ParamVariant maxTransmissionBounces;
    maxTransmissionBounces.InitInt("Renderer", "Indirect Lighting", "Max Glossy Bounces (Transmissive)",
        fastdelegate::MakeDelegate(this, &IndirectLighting::MaxTransmissionBouncesCallback),
        DefaultParamVals::MAX_GLOSSY_BOUNCES_TRANSMISSIVE, 1, 8, 1, "Path Sampling");
    App::AddParam(maxTransmissionBounces);

    ParamVariant pathRegularization;
    pathRegularization.InitBool("Renderer", "Indirect Lighting", "Path Regularization",
        fastdelegate::MakeDelegate(this, &IndirectLighting::PathRegularizationCallback),
        DefaultParamVals::PATH_REGULARIZATION, "Path Sampling");
    App::AddParam(pathRegularization);

    ParamVariant texFilter;
    texFilter.InitEnum("Renderer", "Indirect Lighting", "Texture Filter",
        fastdelegate::MakeDelegate(this, &IndirectLighting::TexFilterCallback),
        Params::TextureFilter, ZetaArrayLen(Params::TextureFilter), (uint32)DefaultParamVals::TEX_FILTER);
    App::AddParam(texFilter);

    m_method = method;
    m_doTemporalResampling = method == INTEGRATOR::PATH_TRACING ? false : m_doTemporalResampling;
    m_doSpatialResampling = method == INTEGRATOR::PATH_TRACING ? false : m_doSpatialResampling;

    ResetIntegrator(true, false);
}

void IndirectLighting::OnWindowResized()
{
    // Since window was resized, recreate all resources, but leave the existing parameters
    // and shader reload handlers
    ResetIntegrator(true, true);
    m_isTemporalReservoirValid = false;
    m_currTemporalIdx = 0;
}

void IndirectLighting::SetMethod(INTEGRATOR method)
{
    const auto old = m_method;
    m_method = method;
    m_doTemporalResampling = method == INTEGRATOR::PATH_TRACING ? false : m_doTemporalResampling;
    m_doSpatialResampling = method == INTEGRATOR::PATH_TRACING ? false : m_doSpatialResampling;

    if (old != m_method)
    {
        if (old == INTEGRATOR::ReSTIR_GI)
            ReleaseReSTIR_GI();
        else if (old == INTEGRATOR::ReSTIR_PT)
            ReleaseReSTIR_PT();

        ResetIntegrator(false, false);
    }
}

void IndirectLighting::RenderPathTracer(Core::ComputeCmdList& computeCmdList)
{
    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.PIXBeginEvent("PathTracer");
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "PathTracer");

    const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_GI_TEMPORAL_GROUP_DIM_X);
    const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_GI_TEMPORAL_GROUP_DIM_Y);
    m_cbRGI.DispatchDimX_NumGroupsInTile = ((RESTIR_GI_TEMPORAL_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;

    Assert(!m_preSampling || m_cbRGI.SampleSetSize_NumSampleSets, "Presampled set params haven't been set.");

    m_cbRGI.FinalOrColorAUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::DNSR_FINAL_UAV);
    m_rootSig.SetRootConstants(0, sizeof(m_cbRGI) / sizeof(DWORD), &m_cbRGI);
    m_rootSig.End(computeCmdList);

    auto sh = App::GetScene().NumEmissiveInstances() > 0 ? SHADER::PATH_TRACER_WoPS :
        SHADER::PATH_TRACER;
    if (m_preSampling)
        sh = SHADER::PATH_TRACER_WPS;

    computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
    computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

    gpuTimer.EndQuery(computeCmdList, queryIdx);
    computeCmdList.PIXEndEvent();
}

void IndirectLighting::RenderReSTIR_GI(ComputeCmdList& computeCmdList)
{
    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    // Spatio-temporal reuse
    {
        computeCmdList.PIXBeginEvent("ReSTIR_GI");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_GI");

        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, Reservoir_RGI::NUM * 2 + 2> textureBarriers;

        // transition current temporal reservoir into write state
        ID3D12Resource* currReservoirs[Reservoir_RGI::NUM] = { m_reservoir_RGI[m_currTemporalIdx].A.Resource(),
            m_reservoir_RGI[m_currTemporalIdx].B.Resource(),
            m_reservoir_RGI[m_currTemporalIdx].C.Resource() };

        for (int i = 0; i < ZetaArrayLen(currReservoirs); i++)
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(currReservoirs[i]));

        // transition color outputs into write state
        if (IS_CB_FLAG_SET(m_cbRGI, CB_IND_FLAGS::DENOISE))
        {
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_colorA.Resource()));
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_colorB.Resource()));
        }

        // transition previous temporal reservoirs into read state
        if (m_isTemporalReservoirValid)
        {
            ID3D12Resource* prevReservoirs[Reservoir_RGI::NUM] = { m_reservoir_RGI[1 - m_currTemporalIdx].A.Resource(),
                m_reservoir_RGI[1 - m_currTemporalIdx].B.Resource(),
                m_reservoir_RGI[1 - m_currTemporalIdx].C.Resource() };

            for (int i = 0; i < ZetaArrayLen(prevReservoirs); i++)
                textureBarriers.push_back(TextureBarrier_UavToSrvNoSync(prevReservoirs[i]));
        }

        computeCmdList.ResourceBarrier(textureBarriers.data(), (UINT)textureBarriers.size());

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_GI_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_GI_TEMPORAL_GROUP_DIM_Y);
        m_cbRGI.DispatchDimX_NumGroupsInTile = ((RESTIR_GI_TEMPORAL_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;

        SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::TEMPORAL_RESAMPLE, m_doTemporalResampling && m_isTemporalReservoirValid);
        SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::SPATIAL_RESAMPLE, m_doSpatialResampling && m_isTemporalReservoirValid);
        Assert(!m_preSampling || m_cbRGI.SampleSetSize_NumSampleSets, "Presampled set params haven't been set.");

        auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_0_A_SRV : DESC_TABLE_RGI::RESERVOIR_1_A_SRV;
        auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_0_B_SRV : DESC_TABLE_RGI::RESERVOIR_1_B_SRV;
        auto srvCIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_0_C_SRV : DESC_TABLE_RGI::RESERVOIR_1_C_SRV;
        auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_1_A_UAV : DESC_TABLE_RGI::RESERVOIR_0_A_UAV;
        auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_1_B_UAV : DESC_TABLE_RGI::RESERVOIR_0_B_UAV;
        auto uavCIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_1_C_UAV : DESC_TABLE_RGI::RESERVOIR_0_C_UAV;

        m_cbRGI.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
        m_cbRGI.PrevReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
        m_cbRGI.PrevReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvCIdx);
        m_cbRGI.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
        m_cbRGI.CurrReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
        m_cbRGI.CurrReservoir_C_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavCIdx);

        m_cbRGI.FinalOrColorAUavDescHeapIdx = IS_CB_FLAG_SET(m_cbRGI, CB_IND_FLAGS::DENOISE) ?
            m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::COLOR_A_UAV) :
            m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::DNSR_FINAL_UAV);
        m_cbRGI.ColorBUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::COLOR_B_UAV);
        m_rootSig.SetRootConstants(0, sizeof(m_cbRGI) / sizeof(DWORD), &m_cbRGI);
        m_rootSig.End(computeCmdList);

        auto sh = App::GetScene().NumEmissiveInstances() > 0 ? SHADER::ReSTIR_GI_WoPS :
            SHADER::ReSTIR_GI;
        if (m_preSampling)
            sh = m_useLVG ? SHADER::ReSTIR_GI_LVG : SHADER::ReSTIR_GI_WPS;

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }

    if (!IS_CB_FLAG_SET(m_cbRGI, CB_IND_FLAGS::DENOISE))
        return;
    
    // denoiser - temporal
    {
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "IndirectDnsrTemporal");
        computeCmdList.PIXBeginEvent("IndirectDnsrTemporal");

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::DNSR_TEMPORAL));

        D3D12_TEXTURE_BARRIER barriers[4];

        // transition color into read state
        barriers[0] = Direct3DUtil::TextureBarrier_UavToSrvWithSync(m_colorA.Resource());
        barriers[1] = Direct3DUtil::TextureBarrier_UavToSrvWithSync(m_colorB.Resource());
        // transition current denoiser caches into write
        barriers[2] = Direct3DUtil::TextureBarrier_SrvToUavNoSync(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource());
        barriers[3] = Direct3DUtil::TextureBarrier_SrvToUavNoSync(m_dnsrCache[m_currTemporalIdx].Specular.Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

        auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV :
            DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV;
        auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV :
            DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV;
        auto uavDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV :
            DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV;
        auto uavSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV :
            DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV;

        m_cbDnsrTemporal.ColorASrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::COLOR_A_SRV);
        m_cbDnsrTemporal.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::COLOR_B_SRV);
        m_cbDnsrTemporal.PrevTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
        m_cbDnsrTemporal.PrevTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
        m_cbDnsrTemporal.CurrTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavDiffuseIdx);
        m_cbDnsrTemporal.CurrTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavSpecularIdx);
        m_cbDnsrTemporal.IsTemporalCacheValid = m_isDnsrTemporalCacheValid;
        m_cbDnsrTemporal.PrevReservoir_A_DescHeapIdx = m_cbRGI.PrevReservoir_A_DescHeapIdx;

        m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrTemporal) / sizeof(DWORD), &m_cbDnsrTemporal);
        m_rootSig.End(computeCmdList);

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, INDIRECT_DNSR_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, INDIRECT_DNSR_TEMPORAL_GROUP_DIM_Y);
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }

    // denoiser - spatial
    {
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "IndirectDnsrSpatial");
        computeCmdList.PIXBeginEvent("IndirectDnsrSpatial");

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::DNSR_SPATIAL));

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, INDIRECT_DNSR_SPATIAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, INDIRECT_DNSR_SPATIAL_GROUP_DIM_Y);

        D3D12_TEXTURE_BARRIER barriers[2];

        // transition color into read state
        barriers[0] = Direct3DUtil::TextureBarrier_UavToSrvWithSync(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource());
        barriers[1] = Direct3DUtil::TextureBarrier_UavToSrvWithSync(m_dnsrCache[m_currTemporalIdx].Specular.Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

        auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV :
            DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV;
        auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV :
            DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV;

        m_cbDnsrSpatial.TemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
        m_cbDnsrSpatial.TemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
        m_cbDnsrSpatial.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::COLOR_B_SRV);
        m_cbDnsrSpatial.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RGI::DNSR_FINAL_UAV);
        m_cbDnsrSpatial.DispatchDimX = (uint16_t)dispatchDimX;
        m_cbDnsrSpatial.DispatchDimY = (uint16_t)dispatchDimY;
        m_cbDnsrSpatial.NumGroupsInTile = INDIRECT_DNSR_SPATIAL_TILE_WIDTH * m_cbDnsrSpatial.DispatchDimY;

        m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrSpatial) / sizeof(DWORD), &m_cbDnsrSpatial);
        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }
}

void IndirectLighting::ReSTIR_PT_Temporal(ComputeCmdList& computeCmdList, 
    Span<ID3D12Resource*> currReservoirs)
{
    Assert(currReservoirs.size() == Reservoir_RPT::NUM, "Invalid #reservoirs.");
    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();
    const bool emissive = App::GetScene().NumEmissiveInstances() > 0;

    computeCmdList.PIXBeginEvent("ReSTIR_PT_Temporal");
    const uint32_t allQueryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_PT_Temporal");

    // Sort - TtC
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_TtC");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y);

        cb_ReSTIR_PT_Sort cb;
        cb.DispatchDimX = dispatchDimX;
        cb.DispatchDimY = dispatchDimY;
        cb.Reservoir_A_DescHeapIdx = m_cbRPT_Temporal.PrevReservoir_A_DescHeapIdx;
        cb.MapDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_NtC_UAV);
        cb.Flags = m_cbRPT_Temporal.Flags;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_TtC));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // UAV barriers for current frame's reservoirs
    {
        D3D12_TEXTURE_BARRIER barriers[Reservoir_RPT::NUM];

        for (int i = 0; i < currReservoirs.size(); i++)
            barriers[i] = UAVBarrier1(currReservoirs[i]);

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
    }

    // Sort - CtT
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_CtT");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y);

        cb_ReSTIR_PT_Sort cb;
        cb.DispatchDimX = dispatchDimX;
        cb.DispatchDimY = dispatchDimY;
        cb.Reservoir_A_DescHeapIdx = m_cbRPT_PathTrace.Reservoir_A_DescHeapIdx;
        cb.MapDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_CtN_UAV);
        cb.Flags = m_cbRPT_Temporal.Flags;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_CtT));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Transition thread maps into read state
    {
        D3D12_TEXTURE_BARRIER barriers[(int)SHIFT::COUNT];
        barriers[(int)SHIFT::CtN] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::CtN].Resource());
        barriers[(int)SHIFT::NtC] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::NtC].Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
    }

    // Replay - CtT
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_CtT");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

        m_cbRPT_Temporal.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
            (int)DESC_TABLE_RPT::RBUFFER_A_CtN_UAV);
        m_cbRPT_Temporal.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
            (int)DESC_TABLE_RPT::RBUFFER_A_NtC_UAV);

        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Temporal) / sizeof(DWORD), &m_cbRPT_Temporal);
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_CtT_E : SHADER::ReSTIR_PT_REPLAY_CtT;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Replay - TtC
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_TtC");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

        auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_TtC_E : SHADER::ReSTIR_PT_REPLAY_TtC;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Set SRVs for replay buffers
    m_cbRPT_Temporal.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::RBUFFER_A_CtN_SRV);
    m_cbRPT_Temporal.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::RBUFFER_A_NtC_SRV);

    // Transition r-buffers into read state
    {
        D3D12_TEXTURE_BARRIER barriers[RBuffer::NUM * (int)SHIFT::COUNT];

        // Transition CtN replay buffers into read state
        barriers[0] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].A.Resource());
        barriers[1] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].B.Resource());
        barriers[2] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].C.Resource());

        // Transition NtC replay buffers into read state
        barriers[3] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].A.Resource());
        barriers[4] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].B.Resource());
        barriers[5] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].C.Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
    }

    // Reconnect CtT
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_CtT");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_TEMPORAL_GROUP_DIM_Y);
        m_cbRPT_Temporal.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;

        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Temporal) / sizeof(DWORD), &m_cbRPT_Temporal);
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_CtT_E : SHADER::ReSTIR_PT_RECONNECT_CtT;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Reconnect TtC
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_TtC");
#endif
        D3D12_TEXTURE_BARRIER uavBarriers[2];

        // UAV barriers for current frame's reservoir B
        uavBarriers[0] = UAVBarrier1(currReservoirs[1]);
        // UAV barriers for target
        uavBarriers[1] = UAVBarrier1(m_rptTarget.Resource());

        computeCmdList.ResourceBarrier(uavBarriers, ZetaArrayLen(uavBarriers));

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_TEMPORAL_GROUP_DIM_Y);

        auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_TtC_E : SHADER::ReSTIR_PT_RECONNECT_TtC;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    gpuTimer.EndQuery(computeCmdList, allQueryIdx);
    computeCmdList.PIXEndEvent();
}

void IndirectLighting::ReSTIR_PT_Spatial(ComputeCmdList& computeCmdList, 
    Span<ID3D12Resource*> currReservoirs,
    Span<ID3D12Resource*> prevReservoirs)
{
    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();
    const bool emissive = App::GetScene().NumEmissiveInstances() > 0;

    computeCmdList.PIXBeginEvent("ReSTIR_PT_Spatial");
    const uint32_t allQueryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_PT_Spatial");

    // Search for reusable spatial neighbor
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_SpatialSearch");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y);

        cb_ReSTIR_PT_SpatialSearch cb;
        cb.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;
        m_cbRPT_Temporal.Packed = (m_cbRPT_Temporal.Packed & 0xffff0fff);
        cb.OutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_UAV);
        cb.Flags = m_cbRPT_Temporal.Flags;
        cb.Final = m_cbRPT_Temporal.Final;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SPATIAL_SEARCH));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Barriers
    {
        constexpr int N = Reservoir_RPT::NUM * 2 + 1 + RBuffer::NUM * (int)SHIFT::COUNT + (int)SHIFT::COUNT;
        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, N> barriers;

        // Transition previous frame's reservoirs into write state
        for (int i = 0; i < Reservoir_RPT::NUM; i++)
            barriers.push_back(TextureBarrier_SrvToUavWithSync(prevReservoirs[i]));

        // Transition current frame's reservoirs into read state
        for (int i = 0; i < Reservoir_RPT::NUM; i++)
            barriers.push_back(TextureBarrier_UavToSrvWithSync(currReservoirs[i]));

        // Transition spatial neighbor idx into read state
        barriers.push_back(TextureBarrier_UavToSrvWithSync(m_spatialNeighbor.Resource()));

        // Transition r-buffers into write state
        for (int i = 0; i < (int)SHIFT::COUNT; i++)
        {
            barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].A.Resource()));
            barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].B.Resource()));
            barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].C.Resource()));
        }

        // Transition thread maps into UAV
        if (IS_CB_FLAG_SET(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_SPATIAL))
        {
            barriers.push_back(TextureBarrier_SrvToUavWithSync(m_threadMap[(int)SHIFT::NtC].Resource()));
            barriers.push_back(TextureBarrier_SrvToUavWithSync(m_threadMap[(int)SHIFT::CtN].Resource()));
        }

        computeCmdList.ResourceBarrier(barriers.data(), (UINT)barriers.size());
    }

    // Sort - CtS
    if (IS_CB_FLAG_SET(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_SPATIAL))
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_CtS");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y);

        cb_ReSTIR_PT_Sort cb;
        cb.DispatchDimX = dispatchDimX;
        cb.DispatchDimY = dispatchDimY;
        cb.Reservoir_A_DescHeapIdx = m_cbRPT_Temporal.Reservoir_A_DescHeapIdx;
        cb.MapDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_CtN_UAV);
        cb.Flags = m_cbRPT_Temporal.Flags;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_CtS));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Sort - StC
    if (IS_CB_FLAG_SET(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_SPATIAL))
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_StC");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y);

        cb_ReSTIR_PT_Sort cb;
        cb.DispatchDimX = dispatchDimX;
        cb.DispatchDimY = dispatchDimY;
        cb.Reservoir_A_DescHeapIdx = m_cbRPT_Temporal.Reservoir_A_DescHeapIdx;
        cb.SpatialNeighborHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_SRV);
        cb.MapDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_NtC_UAV);
        cb.Flags = m_cbRPT_Temporal.Flags;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_StC));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Replay - CtS
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_CtS");
#endif
        // Transition the thread maps into SRV
        if (IS_CB_FLAG_SET(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_SPATIAL))
        {
            D3D12_TEXTURE_BARRIER barriers[(int)SHIFT::COUNT];
            barriers[(int)SHIFT::CtN] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::CtN].Resource());
            barriers[(int)SHIFT::NtC] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::NtC].Resource());

            computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
        }

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

        m_cbRPT_Temporal.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
            (int)DESC_TABLE_RPT::RBUFFER_A_CtN_UAV);
        m_cbRPT_Temporal.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
            (int)DESC_TABLE_RPT::RBUFFER_A_NtC_UAV);

        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Temporal) / sizeof(DWORD), &m_cbRPT_Temporal);
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_CtS_E : SHADER::ReSTIR_PT_REPLAY_CtS;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Replay - StC
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_StC");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

        auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_StC_E : SHADER::ReSTIR_PT_REPLAY_StC;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
    computeCmdList.PIXEndEvent();
#endif
    }

    // Set SRVs for replay buffers
    m_cbRPT_Temporal.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::RBUFFER_A_CtN_SRV);
    m_cbRPT_Temporal.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::RBUFFER_A_NtC_SRV);

    // Transition r-buffers into read state
    {
        D3D12_TEXTURE_BARRIER barriers[RBuffer::NUM * (int)SHIFT::COUNT];

        // CtN 
        barriers[0] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].A.Resource());
        barriers[1] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].B.Resource());
        barriers[2] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].C.Resource());

        // NtC
        barriers[3] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].A.Resource());
        barriers[4] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].B.Resource());
        barriers[5] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].C.Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
    }

    // Reconnect CtS
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_CtS");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SPATIAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SPATIAL_GROUP_DIM_Y);

        m_cbRPT_Temporal.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;
        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Temporal) / sizeof(DWORD), &m_cbRPT_Temporal);
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_CtS_E : SHADER::ReSTIR_PT_RECONNECT_CtS;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Reconnect StC
    {
#ifdef _DEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_StC");
#endif

        // UAV barriers for previous frame's reservoir B
        D3D12_TEXTURE_BARRIER uavBarriers[2];
        uavBarriers[0] = UAVBarrier1(prevReservoirs[1]);
        // UAV barriers for target
        uavBarriers[1] = UAVBarrier1(m_rptTarget.Resource());

        computeCmdList.ResourceBarrier(uavBarriers, ZetaArrayLen(uavBarriers));

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SPATIAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SPATIAL_GROUP_DIM_Y);

        auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_StC_E : SHADER::ReSTIR_PT_RECONNECT_StC;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifdef _DEBUG
    computeCmdList.PIXEndEvent();
#endif
    }

    gpuTimer.EndQuery(computeCmdList, allQueryIdx);
    computeCmdList.PIXEndEvent();
}

void IndirectLighting::RenderReSTIR_PT(Core::ComputeCmdList& computeCmdList)
{
    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    ID3D12Resource* currReservoirs[Reservoir_RPT::NUM] = { m_reservoir_RPT[m_currTemporalIdx].A.Resource(),
        m_reservoir_RPT[m_currTemporalIdx].B.Resource(),
        m_reservoir_RPT[m_currTemporalIdx].C.Resource(),
        m_reservoir_RPT[m_currTemporalIdx].D.Resource(),
        m_reservoir_RPT[m_currTemporalIdx].E.Resource(),
        m_reservoir_RPT[m_currTemporalIdx].F.Resource(),
        m_reservoir_RPT[m_currTemporalIdx].G.Resource() };

    ID3D12Resource* prevReservoirs[Reservoir_RPT::NUM] = { m_reservoir_RPT[1 - m_currTemporalIdx].A.Resource(),
        m_reservoir_RPT[1 - m_currTemporalIdx].B.Resource(),
        m_reservoir_RPT[1 - m_currTemporalIdx].C.Resource(),
        m_reservoir_RPT[1 - m_currTemporalIdx].D.Resource(),
        m_reservoir_RPT[1 - m_currTemporalIdx].E.Resource(),
        m_reservoir_RPT[1 - m_currTemporalIdx].F.Resource(),
        m_reservoir_RPT[1 - m_currTemporalIdx].G.Resource() };

    auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RPT::RESERVOIR_0_A_SRV : 
        DESC_TABLE_RPT::RESERVOIR_1_A_SRV;
    auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RPT::RESERVOIR_1_A_UAV : 
        DESC_TABLE_RPT::RESERVOIR_0_A_UAV;

    const bool doTemporal = m_doTemporalResampling && m_isTemporalReservoirValid;
    const bool doSpatial = m_doSpatialResampling && doTemporal;

    // Initial candidates
    {
        computeCmdList.PIXBeginEvent("ReSTIR_PT_PathTrace");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_PT_PathTrace");

        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, 
            Reservoir_RPT::NUM * 2 + RBuffer::NUM * 2 + (int)SHIFT::COUNT + 1> textureBarriers;

        // Transition current reservoir into write state
        if (m_reservoir_RPT[m_currTemporalIdx].Layout != D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS)
        {
            for (int i = 0; i < ZetaArrayLen(currReservoirs); i++)
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(currReservoirs[i]));

            m_reservoir_RPT[m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;
        }

        if (doTemporal)
        {
            Assert(m_reservoir_RPT[1 - m_currTemporalIdx].Layout == D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                "Unexpected resource layout.");

            // Transition temporal reservoirs into read state
            for (int i = 0; i < ZetaArrayLen(prevReservoirs); i++)
                textureBarriers.push_back(TextureBarrier_UavToSrvNoSync(prevReservoirs[i]));

            m_reservoir_RPT[1 - m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;

            // Transition r-buffers into write state
            for (int i = 0; i < (int)SHIFT::COUNT; i++)
            {
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].A.Resource()));
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].B.Resource()));
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].C.Resource()));
            }

            // Transition thread maps into write state
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_threadMap[(int)SHIFT::CtN].Resource()));
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_threadMap[(int)SHIFT::NtC].Resource()));

            if (doSpatial)
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_spatialNeighbor.Resource()));
        }

        if(!textureBarriers.empty())
            computeCmdList.ResourceBarrier(textureBarriers.data(), (UINT)textureBarriers.size());

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_PATH_TRACE_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_PATH_TRACE_GROUP_DIM_Y);

        SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::TEMPORAL_RESAMPLE, doTemporal);
        SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::SPATIAL_RESAMPLE, doSpatial);
        SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::SPATIAL_RESAMPLE, doSpatial);
        Assert(!m_preSampling || m_cbRPT_PathTrace.SampleSetSize_NumSampleSets, 
            "Presampled set params haven't been set.");

        m_cbRPT_PathTrace.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;
        m_cbRPT_PathTrace.Reservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);

        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_PathTrace) / sizeof(DWORD), &m_cbRPT_PathTrace);
        m_rootSig.End(computeCmdList);

        auto sh = App::GetScene().NumEmissiveInstances() > 0 ? SHADER::ReSTIR_PT_PATH_TRACE_WoPS : 
            SHADER::ReSTIR_PT_PATH_TRACE;
        sh = m_preSampling ? SHADER::ReSTIR_PT_PATH_TRACE_WPS : sh;

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }

    if(doTemporal)
    {
        // Since reservoir descriptors were allocated consecutively, filling just
        // the heap index for A is enough
        m_cbRPT_Temporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
        m_cbRPT_Temporal.Reservoir_A_DescHeapIdx = m_cbRPT_PathTrace.Reservoir_A_DescHeapIdx;

        ReSTIR_PT_Temporal(computeCmdList, currReservoirs);
    }

    if (doSpatial)
    {
        ReSTIR_PT_Spatial(computeCmdList, currReservoirs, prevReservoirs);

        m_reservoir_RPT[1 - m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;
        m_reservoir_RPT[m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
        m_currTemporalIdx = 1 - m_currTemporalIdx;
    }
}

void IndirectLighting::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    if (m_method == INTEGRATOR::ReSTIR_PT)
        RenderReSTIR_PT(computeCmdList);
    else if (m_method == INTEGRATOR::ReSTIR_GI)
        RenderReSTIR_GI(computeCmdList);
    else
        RenderPathTracer(computeCmdList);

    m_isTemporalReservoirValid = true;
    m_currTemporalIdx = 1 - m_currTemporalIdx;
    m_isDnsrTemporalCacheValid = m_cbDnsrTemporal.Denoise;
}

void IndirectLighting::SwitchToReSTIR_PT(bool skipNonResources)
{
    auto& renderer = App::GetRenderer();
    const auto w = renderer.GetRenderWidth();
    const auto h = renderer.GetRenderHeight();

    // Make sure offset between index 0 and index 1 descriptors is always the same
    static_assert((int)(DESC_TABLE_RPT::RESERVOIR_0_B_SRV) +
        Reservoir_RPT::NUM * 2 == (int)DESC_TABLE_RPT::RESERVOIR_1_B_SRV);
    static_assert((int)(DESC_TABLE_RPT::RESERVOIR_0_C_UAV) +
        Reservoir_RPT::NUM * 2 == (int)DESC_TABLE_RPT::RESERVOIR_1_C_UAV);

    // Reservoirs (double buffered) + 2 thread maps + 2 r-buffers
    constexpr int N = 2 * Reservoir_RPT::NUM + (int)SHIFT::COUNT + 2 + (int)SHIFT::COUNT * RBuffer::NUM;
    PlacedResourceList<N> list;

    for (int i = 0; i < 2; i++)
    {
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_C, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_D, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_E, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_F, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RESERVOIR_G, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    for (int i = 0; i < (int)SHIFT::COUNT; i++)
        list.PushTex2D(ResourceFormats_RPT::THREAD_MAP, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    list.PushTex2D(ResourceFormats_RPT::SPATIAL_NEIGHBOR, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    list.PushTex2D(ResourceFormats_RPT::TARGET, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    for (int i = 0; i < (int)SHIFT::COUNT; i++)
    {
        list.PushTex2D(ResourceFormats_RPT::RBUFFER_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RBUFFER_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RPT::RBUFFER_C, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    list.End();

    m_resHeap = GpuMemory::GetResourceHeap(list.Size());
    auto allocs = list.AllocInfos();
    int currRes = 0;
    const auto initState0 = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;
    const auto initState1 = m_doTemporalResampling ?
        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE :
        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;

    auto func = [this, w, h](Texture& tex, DXGI_FORMAT format, const char* baseName, int idx,
        const char* subName, const D3D12_RESOURCE_ALLOCATION_INFO1& allocInfo,
        int srvIdx, int uavIdx, int descOffset, D3D12_BARRIER_LAYOUT layout)
        {
            StackStr(name, N, "%s_%d_%s", baseName, idx, subName);
            tex = GpuMemory::GetPlacedTexture2D(name, w, h, format, m_resHeap.Heap(), allocInfo.Offset,
                layout, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

            if (srvIdx != -1)
                Direct3DUtil::CreateTexture2DSRV(tex, m_descTable.CPUHandle(srvIdx + descOffset));

            Direct3DUtil::CreateTexture2DUAV(tex, m_descTable.CPUHandle(uavIdx + descOffset));
        };

    // Reservoirs
    for (int i = 0; i < 2; i++)
    {
        const auto state = i == 0 ? initState0 : initState1;
        const int descOffset = i * Reservoir_RPT::NUM * 2;

        func(m_reservoir_RPT[i].A, ResourceFormats_RPT::RESERVOIR_A,
            "RPT_Reservoir", i, "A", allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_A_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_A_UAV, 
            descOffset, state);
        func(m_reservoir_RPT[i].B, ResourceFormats_RPT::RESERVOIR_B,
            "RPT_Reservoir", i, "B", allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_B_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_B_UAV,
            descOffset, state);
        func(m_reservoir_RPT[i].C, ResourceFormats_RPT::RESERVOIR_C,
            "RPT_Reservoir", i, "C", allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_C_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_C_UAV,
            descOffset, state);
        func(m_reservoir_RPT[i].D, ResourceFormats_RPT::RESERVOIR_D,
            "RPT_Reservoir", i, "D", allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_D_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_D_UAV,
            descOffset, state);
        func(m_reservoir_RPT[i].E, ResourceFormats_RPT::RESERVOIR_E,
            "RPT_Reservoir", i, "E",  allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_E_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_E_UAV,
            descOffset, state);
        func(m_reservoir_RPT[i].F, ResourceFormats_RPT::RESERVOIR_F,
            "RPT_Reservoir", i, "F", allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_F_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_F_UAV,
            descOffset, state);
        func(m_reservoir_RPT[i].G, ResourceFormats_RPT::RESERVOIR_G,
            "RPT_Reservoir", i, "G", allocs[currRes++],
            (int)DESC_TABLE_RPT::RESERVOIR_0_G_SRV, (int)DESC_TABLE_RPT::RESERVOIR_0_G_UAV,
            descOffset, state);
    }

    m_reservoir_RPT[0].Layout = initState0;
    m_reservoir_RPT[1].Layout = initState1;

    // Thread Maps
    for (int i = 0; i < (int)SHIFT::COUNT; i++)
    {
        const int descOffset = i * (int)SHIFT::COUNT;

        func(m_threadMap[i], ResourceFormats_RPT::THREAD_MAP,
            "RPT_Map", i, "", allocs[currRes++], 
            (int)DESC_TABLE_RPT::THREAD_MAP_CtN_SRV, (int)DESC_TABLE_RPT::THREAD_MAP_CtN_UAV, 
            descOffset, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);
    }

    // Spatial neighbor 
    func(m_spatialNeighbor, ResourceFormats_RPT::SPATIAL_NEIGHBOR,
        "RPT_SpatialNeighbor", 0, "", allocs[currRes++], 
        (int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_SRV, (int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_UAV, 
        0, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);

    // Target
    func(m_rptTarget, ResourceFormats_RPT::TARGET, "RPT_Target", 0, "",
        allocs[currRes++], -1, (int)DESC_TABLE_RPT::TARGET_UAV, 0,
        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS);

    // R-Buffers
    // Make sure offset between CtN and NtC descriptors is always the same
    static_assert((int)(DESC_TABLE_RPT::RBUFFER_A_CtN_SRV) +
        RBuffer::NUM * 2 == (int)DESC_TABLE_RPT::RBUFFER_A_NtC_SRV);
    static_assert((int)(DESC_TABLE_RPT::RBUFFER_B_CtN_UAV) +
        RBuffer::NUM * 2 == (int)DESC_TABLE_RPT::RBUFFER_B_NtC_UAV);
    static_assert((int)(DESC_TABLE_RPT::RBUFFER_C_CtN_UAV) +
        RBuffer::NUM * 2 == (int)DESC_TABLE_RPT::RBUFFER_C_NtC_UAV);

    for (int i = 0; i < (int)SHIFT::COUNT; i++)
    {
        const int descOffset = i * RBuffer::NUM * 2;
        const auto layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;

        func(m_rbuffer[i].A, ResourceFormats_RPT::RBUFFER_A,
            "RPT_RBuffer", i, i == (int)SHIFT::CtN ? "A_CtN" : "A_NtC", 
            allocs[currRes++],
            (int)DESC_TABLE_RPT::RBUFFER_A_CtN_SRV, 
            (int)DESC_TABLE_RPT::RBUFFER_A_CtN_UAV,
            descOffset, layout);
        func(m_rbuffer[i].B, ResourceFormats_RPT::RBUFFER_B,
            "RPT_RBuffer", i,  i == (int)SHIFT::CtN ? "B_CtN" : "B_NtC", 
            allocs[currRes++],
            (int)DESC_TABLE_RPT::RBUFFER_B_CtN_SRV,
            (int)DESC_TABLE_RPT::RBUFFER_B_CtN_UAV,
            descOffset, layout);
        func(m_rbuffer[i].C, ResourceFormats_RPT::RBUFFER_C,
            "RPT_RBuffer", i, i == (int)SHIFT::CtN ? "C_CtN" : "C_NtC", 
            allocs[currRes++],
            (int)DESC_TABLE_RPT::RBUFFER_C_CtN_SRV,
            (int)DESC_TABLE_RPT::RBUFFER_C_CtN_UAV,
            descOffset, layout);
    }

    // Final
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE_RPT::FINAL_UAV));

    // Following never change, so can be set only once
    m_cbRPT_PathTrace.TargetDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::TARGET_UAV);
        m_cbRPT_Temporal.ThreadMap_NtC_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
            (int)DESC_TABLE_RPT::THREAD_MAP_NtC_SRV);
    m_cbRPT_PathTrace.Final = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE_RPT::FINAL_UAV);
    m_cbRPT_Temporal.ThreadMap_CtN_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::THREAD_MAP_CtN_SRV);
    m_cbRPT_Temporal.ThreadMap_NtC_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::THREAD_MAP_NtC_SRV);
    m_cbRPT_Temporal.SpatialNeighborHeapIdx = m_descTable.GPUDesciptorHeapIndex(
        (int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_SRV);
    m_cbRPT_Temporal.TargetDescHeapIdx = m_cbRPT_PathTrace.TargetDescHeapIdx;
    m_cbRPT_Temporal.Final = m_cbRPT_PathTrace.Final;

    // Add ReSTIR PT parameters and shader reload handlers
    if (!skipNonResources)
    {
        App::AddShaderReloadHandler("ReSTIR_PT_PathTrace", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRPT_PathTrace));
        App::AddShaderReloadHandler("ReSTIR_PT_Temporal", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRPT_Temporal));
        App::AddShaderReloadHandler("ReSTIR_PT_Spatial", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRPT_Spatial));

        ParamVariant alphaMin;
        alphaMin.InitFloat("Renderer", "Indirect Lighting", "Alpha_min",
            fastdelegate::MakeDelegate(this, &IndirectLighting::AlphaMinCallback),
            DefaultParamVals::ROUGHNESS_MIN, 0.0f, 1.0f, 1e-2f, "Reuse");
        App::AddParam(alphaMin);

        ParamVariant p2;
        p2.InitEnum("Renderer", "Indirect Lighting", "Debug View", 
            fastdelegate::MakeDelegate(this, &IndirectLighting::DebugViewCallback),
            Params::DebugView, ZetaArrayLen(Params::DebugView), 0);
        App::AddParam(p2);

        ParamVariant doSpatial;
        doSpatial.InitBool("Renderer", "Indirect Lighting", "Spatial Resample",
            fastdelegate::MakeDelegate(this, &IndirectLighting::SpatialResamplingCallback), 
            m_doSpatialResampling, "Reuse");
        App::AddParam(doSpatial);

        ParamVariant sortTemporal;
        sortTemporal.InitBool("Renderer", "Indirect Lighting", "Sort (Temporal)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::SortTemporalCallback), true, "Reuse");
        App::AddParam(sortTemporal);

        ParamVariant sortSpatial;
        sortSpatial.InitBool("Renderer", "Indirect Lighting", "Sort (Spatial)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::SortSpatialCallback), false, "Reuse");
        App::AddParam(sortSpatial);

        ParamVariant doTemporal;
        doTemporal.InitBool("Renderer", "Indirect Lighting", "Temporal Resample",
            fastdelegate::MakeDelegate(this, &IndirectLighting::TemporalResamplingCallback),
            m_doTemporalResampling, "Reuse");
        App::AddParam(doTemporal);

        ParamVariant maxTemporalM;
        maxTemporalM.InitInt("Renderer", "Indirect Lighting", "M_max (Temporal)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::M_maxTCallback),
            DefaultParamVals::M_MAX, 1, 15, 1, "Reuse");
        App::AddParam(maxTemporalM);

        ParamVariant maxSpatialM;
        maxSpatialM.InitInt("Renderer", "Indirect Lighting", "M_max (Spatial)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::M_maxSCallback),
            DefaultParamVals::M_MAX_SPATIAL, 1, 15, 1, "Reuse");
        App::AddParam(maxSpatialM);

        ParamVariant suppressOutliers;
        suppressOutliers.InitBool("Renderer", "Indirect Lighting", "Boiling Suppression",
            fastdelegate::MakeDelegate(this, &IndirectLighting::BoilingSuppressionCallback),
            DefaultParamVals::BOILING_SUPPRESSION, "Reuse");
        App::AddParam(suppressOutliers);

        ParamVariant rejectOutliers;
        rejectOutliers.InitBool("Renderer", "Indirect Lighting", "Reject Outlier Samples",
            fastdelegate::MakeDelegate(this, &IndirectLighting::RejectOutliersCallback),
            false, "Reuse");
        App::AddParam(rejectOutliers);
    }
}

void IndirectLighting::ReleaseReSTIR_PT()
{
    // Release resources
    for (int i = 0; i < 2; i++)
    {
        m_reservoir_RPT[i].A.Reset();
        m_reservoir_RPT[i].B.Reset();
        m_reservoir_RPT[i].C.Reset();
        m_reservoir_RPT[i].D.Reset();
        m_reservoir_RPT[i].E.Reset();
        m_reservoir_RPT[i].F.Reset();

        m_rbuffer[i].A.Reset();
        m_rbuffer[i].B.Reset();
        m_rbuffer[i].C.Reset();

        m_threadMap[i].Reset();
    }

    m_spatialNeighbor.Reset();
    m_rptTarget.Reset();
    m_resHeap.Reset();

    // Remove parameters and shader reload handlers
    App::RemoveShaderReloadHandler("ReSTIR_PT_PathTrace");
    App::RemoveShaderReloadHandler("ReSTIR_PT_Temporal");
    App::RemoveShaderReloadHandler("ReSTIR_PT_Spatial");

    App::RemoveParam("Renderer", "Indirect Lighting", "Alpha_min");
    App::RemoveParam("Renderer", "Indirect Lighting", "Debug View");
    App::RemoveParam("Renderer", "Indirect Lighting", "M_max (Temporal)");
    App::RemoveParam("Renderer", "Indirect Lighting", "M_max (Spatial)");
    App::RemoveParam("Renderer", "Indirect Lighting", "Spatial Resample");
    App::RemoveParam("Renderer", "Indirect Lighting", "Wavefront");
    App::RemoveParam("Renderer", "Indirect Lighting", "Sort (Temporal)");
    App::RemoveParam("Renderer", "Indirect Lighting", "Sort (Spatial)");
    App::RemoveParam("Renderer", "Indirect Lighting", "Temporal Resample");
    App::RemoveParam("Renderer", "Indirect Lighting", "Boiling Suppression");
    App::RemoveParam("Renderer", "Indirect Lighting", "Reject Outlier Samples");
}

void IndirectLighting::SwitchToReSTIR_GI(bool skipNonResources)
{
    auto& renderer = App::GetRenderer();
    const auto w = renderer.GetRenderWidth();
    const auto h = renderer.GetRenderHeight();

    // Create reservoirs and their descriptors
    static_assert((int)(DESC_TABLE_RGI::RESERVOIR_0_A_SRV) +
        Reservoir_RGI::NUM * 2 == (int)DESC_TABLE_RGI::RESERVOIR_1_A_SRV);
    static_assert((int)(DESC_TABLE_RGI::RESERVOIR_0_A_UAV) +
        Reservoir_RGI::NUM * 2 == (int)DESC_TABLE_RGI::RESERVOIR_1_A_UAV);

    PlacedResourceList<2 * Reservoir_RGI::NUM + 2 + 2 * 2> list;

    for (int i = 0; i < 2; i++)
    {
        list.PushTex2D(ResourceFormats_RGI::RESERVOIR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RGI::RESERVOIR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RGI::RESERVOIR_C, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }
    
    list.PushTex2D(ResourceFormats_RGI::COLOR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    list.PushTex2D(ResourceFormats_RGI::COLOR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    for (int i = 0; i < 2; i++)
    {
        list.PushTex2D(ResourceFormats_RGI::DNSR_TEMPORAL_CACHE, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RGI::DNSR_TEMPORAL_CACHE, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    list.End();

    m_resHeap = GpuMemory::GetResourceHeap(list.Size());
    auto allocs = list.AllocInfos();
    int currRes = 0;

    auto func = [this, w, h](Texture& tex, DXGI_FORMAT format, const char* baseName, int idx,
        const char* subName, const D3D12_RESOURCE_ALLOCATION_INFO1& allocInfo,
        int srvIdx, int uavIdx, int descOffset, D3D12_BARRIER_LAYOUT layout)
        {
            StackStr(name, N, "%s_%d_%s", baseName, idx, subName);
            tex = GpuMemory::GetPlacedTexture2D(name, w, h, format, m_resHeap.Heap(), allocInfo.Offset,
                layout, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

            Direct3DUtil::CreateTexture2DSRV(tex, m_descTable.CPUHandle(srvIdx + descOffset));
            Direct3DUtil::CreateTexture2DUAV(tex, m_descTable.CPUHandle(uavIdx + descOffset));
        };

    // Reservoirs
    for (int i = 0; i < 2; i++)
    {
        const auto layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
        const int descOffset = i * Reservoir_RGI::NUM * 2;

        func(m_reservoir_RGI[i].A, ResourceFormats_RGI::RESERVOIR_A,
            "RGI_Reservoir", i, "A", allocs[currRes++],
            (int)DESC_TABLE_RGI::RESERVOIR_0_A_SRV, (int)DESC_TABLE_RGI::RESERVOIR_0_A_UAV,
            descOffset, layout);
        func(m_reservoir_RGI[i].B, ResourceFormats_RGI::RESERVOIR_B,
            "RGI_Reservoir", i, "B", allocs[currRes++],
            (int)DESC_TABLE_RGI::RESERVOIR_0_B_SRV, (int)DESC_TABLE_RGI::RESERVOIR_0_B_UAV,
            descOffset, layout);
        func(m_reservoir_RGI[i].C, ResourceFormats_RGI::RESERVOIR_C,
            "RGI_Reservoir", i, "C", allocs[currRes++],
            (int)DESC_TABLE_RGI::RESERVOIR_0_C_SRV, (int)DESC_TABLE_RGI::RESERVOIR_0_C_UAV,
            descOffset, layout);
    }

    // denoiser
    func(m_colorA, ResourceFormats_RGI::COLOR_A, "RGI_COLOR", 0, "A", allocs[currRes++], 
        (int)DESC_TABLE_RGI::COLOR_A_SRV, (int)DESC_TABLE_RGI::COLOR_A_UAV, 
        0, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);
    func(m_colorB, ResourceFormats_RGI::COLOR_B, "RGI_COLOR", 0, "B", allocs[currRes++],
        (int)DESC_TABLE_RGI::COLOR_B_SRV, (int)DESC_TABLE_RGI::COLOR_B_UAV, 
        0, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);

    for (int i = 0; i < 2; i++)
    {
        const auto layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
        const int descOffset = i * 4;

        func(m_dnsrCache[i].Diffuse, ResourceFormats_RGI::DNSR_TEMPORAL_CACHE, 
            "IndirectDnsr", i, "Diffuse", allocs[currRes++], 
            (int)DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, 
            (int)DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV,
            descOffset, layout);

        func(m_dnsrCache[i].Specular, ResourceFormats_RGI::DNSR_TEMPORAL_CACHE,
            "IndirectDnsr", i, "Specular", allocs[currRes++],
            (int)DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV,
            (int)DESC_TABLE_RGI::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV,
            descOffset, layout);
    }

    // Final
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE_RGI::DNSR_FINAL_UAV));

    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, DefaultParamVals::STOCHASTIC_MULTI_BOUNCE);
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::DENOISE, true);
    m_cbDnsrTemporal.Denoise = m_cbDnsrSpatial.Denoise = true;
    m_cbDnsrSpatial.FilterDiffuse = true;
    m_cbDnsrSpatial.FilterSpecular = true;

    // Add ReSTIR GI parameters and shader reload handlers
    if (!skipNonResources)
    {
        ParamVariant stochasticMultibounce;
        stochasticMultibounce.InitBool("Renderer", "Indirect Lighting", "Stochastic Multi-bounce",
            fastdelegate::MakeDelegate(this, &IndirectLighting::StochasticMultibounceCallback),
            DefaultParamVals::STOCHASTIC_MULTI_BOUNCE, "Path Sampling");
        App::AddParam(stochasticMultibounce);

        ParamVariant denoise;
        denoise.InitBool("Renderer", "Indirect Lighting", "Denoise",
            fastdelegate::MakeDelegate(this, &IndirectLighting::DenoiseCallback), true, "Denoiser");
        App::AddParam(denoise);

        ParamVariant tsppDiffuse;
        tsppDiffuse.InitInt("Renderer", "Indirect Lighting", "TSPP (Diffuse)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::TsppDiffuseCallback),
            m_cbDnsrTemporal.MaxTsppDiffuse, 1, 32, 1, "Denoiser");
        App::AddParam(tsppDiffuse);

        ParamVariant tsppSpecular;
        tsppSpecular.InitInt("Renderer", "Indirect Lighting", "TSPP (Specular)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::TsppSpecularCallback),
            m_cbDnsrTemporal.MaxTsppSpecular, 1, 32, 1, "Denoiser");
        App::AddParam(tsppSpecular);

        ParamVariant dnsrSpatialFilterDiffuse;
        dnsrSpatialFilterDiffuse.InitBool("Renderer", "Indirect Lighting", "Spatial Filter (Diffuse)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::DnsrSpatialFilterDiffuseCallback), 
            m_cbDnsrSpatial.FilterDiffuse, "Denoiser");
        App::AddParam(dnsrSpatialFilterDiffuse);

        ParamVariant dnsrSpatialFilterSpecular;
        dnsrSpatialFilterSpecular.InitBool("Renderer", "Indirect Lighting", "Spatial Filter (Specular)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::DnsrSpatialFilterSpecularCallback), 
            m_cbDnsrSpatial.FilterSpecular, "Denoiser");
        App::AddParam(dnsrSpatialFilterSpecular);

        ParamVariant doTemporal;
        doTemporal.InitBool("Renderer", "Indirect Lighting", "Temporal Resample",
            fastdelegate::MakeDelegate(this, &IndirectLighting::TemporalResamplingCallback),
            m_doTemporalResampling, "Reuse");
        App::AddParam(doTemporal);

        ParamVariant maxM;
        maxM.InitInt("Renderer", "Indirect Lighting", "M_max (Temporal)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::M_maxTCallback),
            DefaultParamVals::M_MAX, 1, 15, 1, "Reuse");
        App::AddParam(maxM);

        ParamVariant suppressOutliers;
        suppressOutliers.InitBool("Renderer", "Indirect Lighting", "Boiling Suppression",
            fastdelegate::MakeDelegate(this, &IndirectLighting::BoilingSuppressionCallback),
            DefaultParamVals::BOILING_SUPPRESSION, "Reuse");
        App::AddParam(suppressOutliers);

        App::AddShaderReloadHandler("ReSTIR_GI", fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRGI));
    }
}

void IndirectLighting::ReleaseReSTIR_GI()
{
    for (int i = 0; i < 2; i++)
    {
        m_reservoir_RGI[i].A.Reset();
        m_reservoir_RGI[i].B.Reset();
        m_reservoir_RGI[i].C.Reset();
        
        m_dnsrCache[i].Diffuse.Reset();
        m_dnsrCache[i].Specular.Reset();
    }

    m_colorA.Reset();
    m_colorB.Reset();

    m_resHeap.Reset();

    App::RemoveShaderReloadHandler("ReSTIR_GI");
    App::RemoveParam("Renderer", "Indirect Lighting", "Stochastic Multi-bounce");
    App::RemoveParam("Renderer", "Indirect Lighting", "M_max (Temporal)");
    App::RemoveParam("Renderer", "Indirect Lighting", "Boiling Suppression");
    App::RemoveParam("Renderer", "Indirect Lighting", "Temporal Resample");
    App::RemoveParam("Renderer", "Indirect Lighting", "Denoise");
    App::RemoveParam("Renderer", "Indirect Lighting", "TSPP (Diffuse)");
    App::RemoveParam("Renderer", "Indirect Lighting", "TSPP (Specular)");
    App::RemoveParam("Renderer", "Indirect Lighting", "Spatial Filter (Diffuse)");
    App::RemoveParam("Renderer", "Indirect Lighting", "Spatial Filter (Specular)");
}

void IndirectLighting::SwitchToPathTracer(bool skipNonResources)
{
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE_RGI::DNSR_FINAL_UAV));
}

void IndirectLighting::ResetIntegrator(bool resetAllResources, bool skipNonResources)
{
    auto& renderer = App::GetRenderer();
    const auto w = renderer.GetRenderWidth();
    const auto h = renderer.GetRenderHeight();

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate(m_method == INTEGRATOR::ReSTIR_PT ?
        (int)DESC_TABLE_RPT::COUNT : (int)DESC_TABLE_RGI::COUNT);

    if (resetAllResources || !m_final.IsInitialized())
    {
        m_final = GpuMemory::GetTexture2D("IndirectFinal",
            w, h,
            ResourceFormats_RGI::DNSR_TEMPORAL_CACHE,
            D3D12_RESOURCE_STATE_COMMON,
            TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    if (m_method == INTEGRATOR::ReSTIR_PT)
        SwitchToReSTIR_PT(skipNonResources);
    else if (m_method == INTEGRATOR::ReSTIR_GI)
        SwitchToReSTIR_GI(skipNonResources);
    else
        SwitchToPathTracer(skipNonResources);

    m_isTemporalReservoirValid = false;
    m_currTemporalIdx = 0;
}

void IndirectLighting::MaxDiffuseBouncesCallback(const Support::ParamVariant& p)
{
    const auto newVal = (uint16_t)p.GetInt().m_value;
    m_cbRGI.MaxDiffuseBounces = newVal;
    m_cbRPT_PathTrace.Packed = m_cbRPT_Temporal.Packed = (m_cbRPT_Temporal.Packed & 0xfffff000) |
        (m_cbRGI.MaxDiffuseBounces |
        (m_cbRGI.MaxGlossyBounces_NonTr << 4) |
        (m_cbRGI.MaxGlossyBounces_Tr << 8));
}

void IndirectLighting::MaxGlossyBouncesCallback(const Support::ParamVariant& p)
{
    const auto newVal = (uint16_t)p.GetInt().m_value;
    m_cbRGI.MaxGlossyBounces_NonTr = newVal;
    m_cbRPT_PathTrace.Packed = m_cbRPT_Temporal.Packed = (m_cbRPT_Temporal.Packed & 0xfffff000) |
        (m_cbRGI.MaxDiffuseBounces |
        (m_cbRGI.MaxGlossyBounces_NonTr << 4) |
        (m_cbRGI.MaxGlossyBounces_Tr << 8));
}

void IndirectLighting::MaxTransmissionBouncesCallback(const Support::ParamVariant& p)
{
    const auto newVal = (uint16_t)p.GetInt().m_value;
    m_cbRGI.MaxGlossyBounces_Tr = newVal;
    m_cbRPT_PathTrace.Packed = m_cbRPT_Temporal.Packed = (m_cbRPT_Temporal.Packed & 0xfffff000) |
        (m_cbRGI.MaxDiffuseBounces |
        (m_cbRGI.MaxGlossyBounces_NonTr << 4) |
        (m_cbRGI.MaxGlossyBounces_Tr << 8));
}

void IndirectLighting::StochasticMultibounceCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, p.GetBool());
}

void IndirectLighting::RussianRouletteCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::RUSSIAN_ROULETTE, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::RUSSIAN_ROULETTE, p.GetBool());
}

void IndirectLighting::TemporalResamplingCallback(const Support::ParamVariant& p)
{
    m_doTemporalResampling = p.GetBool();
}

void IndirectLighting::SpatialResamplingCallback(const Support::ParamVariant& p)
{
    m_doSpatialResampling = p.GetBool();
}

void IndirectLighting::M_maxTCallback(const Support::ParamVariant& p)
{
    auto newM = (uint16_t)p.GetInt().m_value;
    m_cbRGI.M_max = newM;
    m_cbRPT_PathTrace.Packed = (m_cbRPT_PathTrace.Packed & 0xfff0ffff) | (newM << 16);
    m_cbRPT_Temporal.Packed = m_cbRPT_PathTrace.Packed;
}

void IndirectLighting::M_maxSCallback(const Support::ParamVariant& p)
{
    auto newM = (uint16_t)p.GetInt().m_value;
    m_cbRPT_Temporal.MaxSpatialM = p.GetInt().m_value;
}

 void IndirectLighting::RejectOutliersCallback(const Support::ParamVariant& p)
 {
     SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::REJECT_OUTLIERS, p.GetBool());
     SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::REJECT_OUTLIERS, p.GetBool());
 }

void IndirectLighting::DebugViewCallback(const Support::ParamVariant& p)
{
    auto newVal = (uint16_t)p.GetEnum().m_curr;
    m_cbRPT_PathTrace.Packed = (m_cbRPT_PathTrace.Packed & 0xfffff) | (newVal << 20);
    m_cbRPT_Temporal.Packed = m_cbRPT_PathTrace.Packed;
}

void IndirectLighting::SortTemporalCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::SORT_TEMPORAL, p.GetBool());
    SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_TEMPORAL, p.GetBool());
}

void IndirectLighting::SortSpatialCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::SORT_SPATIAL, p.GetBool());
}

void IndirectLighting::TexFilterCallback(const Support::ParamVariant& p)
{
    m_cbRPT_PathTrace.TexFilterDescHeapIdx = EnumToSamplerIdx((TEXTURE_FILTER)p.GetEnum().m_curr);
    m_cbRGI.TexFilterDescHeapIdx = m_cbRPT_Temporal.TexFilterDescHeapIdx = 
        m_cbRPT_PathTrace.TexFilterDescHeapIdx;
}

void IndirectLighting::DenoiseCallback(const Support::ParamVariant& p)
{
    m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::DENOISE, p.GetBool());
    m_cbDnsrTemporal.Denoise = p.GetBool();
    m_cbDnsrSpatial.Denoise = p.GetBool();
    m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
}

void IndirectLighting::TsppDiffuseCallback(const Support::ParamVariant& p)
{
    m_cbDnsrTemporal.MaxTsppDiffuse = (uint16_t)p.GetInt().m_value;
    m_cbDnsrSpatial.MaxTsppDiffuse = (uint16_t)p.GetInt().m_value;
}

void IndirectLighting::TsppSpecularCallback(const Support::ParamVariant& p)
{
    m_cbDnsrTemporal.MaxTsppSpecular = (uint16_t)p.GetInt().m_value;
    m_cbDnsrSpatial.MaxTsppSpecular = (uint16_t)p.GetInt().m_value;
}

void IndirectLighting::BoilingSuppressionCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::BOILING_SUPPRESSION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::BOILING_SUPPRESSION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::BOILING_SUPPRESSION, p.GetBool());
}

void IndirectLighting::PathRegularizationCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::PATH_REGULARIZATION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::PATH_REGULARIZATION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_Temporal, CB_IND_FLAGS::PATH_REGULARIZATION, p.GetBool());
}

void IndirectLighting::AlphaMinCallback(const Support::ParamVariant& p)
{
    float newVal = p.GetFloat().m_value;
    m_cbRPT_PathTrace.Alpha_min = m_cbRPT_Temporal.Alpha_min = newVal * newVal;
}

void IndirectLighting::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p)
{
    m_cbDnsrSpatial.FilterDiffuse = p.GetBool();
}

void IndirectLighting::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p)
{
    m_cbDnsrSpatial.FilterSpecular = p.GetBool();
}

void IndirectLighting::ReloadRGI()
{
    auto sh = SHADER::ReSTIR_GI;
    const char* p = "IndirectLighting\\ReSTIR_GI\\ReSTIR_GI.hlsl";

    if (App::GetScene().NumEmissiveInstances() > 0)
    {
        p = "IndirectLighting\\ReSTIR_GI\\Variants\\ReSTIR_GI_WoPS.hlsl";
        sh = SHADER::ReSTIR_GI_WoPS;

        if (m_preSampling)
        {
            p = "IndirectLighting\\ReSTIR_GI\\Variants\\ReSTIR_GI_WPS.hlsl";
            sh = SHADER::ReSTIR_GI_WPS;

            if (m_useLVG)
            {
                p = "IndirectLighting\\ReSTIR_GI\\Variants\\ReSTIR_GI_LVG.hlsl";
                sh = SHADER::ReSTIR_GI_LVG;
            }
        }
    }

    const int i = (int)sh;    
    m_psoLib.Reload(i, m_rootSigObj.Get(), p);
}

void IndirectLighting::ReloadRPT_PathTrace()
{
    auto sh = SHADER::ReSTIR_PT_PATH_TRACE;
    const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_PathTrace.hlsl";

    if (App::GetScene().NumEmissiveInstances() > 0)
    {
        p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_PathTrace_WoPS.hlsl";
        sh = SHADER::ReSTIR_PT_PATH_TRACE_WoPS;

        if (m_preSampling)
        {
            p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_PathTrace_WPS.hlsl";
            sh = SHADER::ReSTIR_PT_PATH_TRACE_WPS;
        }
    }

    const int i = (int)sh;
    m_psoLib.Reload(i, m_rootSigObj.Get(), p);
}

void IndirectLighting::ReloadRPT_Temporal()
{
    {
        auto sh = SHADER::ReSTIR_PT_RECONNECT_CtT;
        const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_CtT.hlsl";
        if (App::GetScene().NumEmissiveInstances() > 0)
        {
            sh = SHADER::ReSTIR_PT_RECONNECT_CtT_E;
            p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_CtT_E.hlsl";
        }
        const int i = (int)sh;
        m_psoLib.Reload(i, m_rootSigObj.Get(), p);
    }

    {
        auto sh = SHADER::ReSTIR_PT_RECONNECT_TtC;
        const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_TtC.hlsl";

        if (App::GetScene().NumEmissiveInstances() > 0)
        {
            sh = SHADER::ReSTIR_PT_RECONNECT_TtC_E;
            p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_TtC_E.hlsl";
        }

        const int i = (int)sh;
        m_psoLib.Reload(i, m_rootSigObj.Get(), p);
    }
}

void IndirectLighting::ReloadRPT_Spatial()
{
    {
       auto sh = SHADER::ReSTIR_PT_RECONNECT_CtS;
       const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_CtS.hlsl";
       if (App::GetScene().NumEmissiveInstances() > 0)
       {
           sh = SHADER::ReSTIR_PT_RECONNECT_CtS_E;
           p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_CtS_E.hlsl";
       }
       const int i = (int)sh;
       m_psoLib.Reload(i, m_rootSigObj.Get(), p);
    }

    {
        auto sh = SHADER::ReSTIR_PT_RECONNECT_StC;
        const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_StC.hlsl";

        if (App::GetScene().NumEmissiveInstances() > 0)
        {
            sh = SHADER::ReSTIR_PT_RECONNECT_StC_E;
            p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_StC_E.hlsl";
        }

        const int i = (int)sh;
        m_psoLib.Reload(i, m_rootSigObj.Get(), p);
    }
}

//void IndirectLighting::ReloadDnsrTemporal()
//{
//    const int i = (int)SHADER::DNSR_TEMPORAL;
//
//    m_psoLib.Reload(i, "IndirectLighting\\IndirectDnsr_Temporal.hlsl", true);
//    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
//}
//
//void IndirectLighting::ReloadDnsrSpatial()
//{
//    const int i = (int)SHADER::DNSR_SPATIAL;
//
//    m_psoLib.Reload(i, "IndirectLighting\\IndirectDnsr_Spatial.hlsl", true);
//    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
//}
