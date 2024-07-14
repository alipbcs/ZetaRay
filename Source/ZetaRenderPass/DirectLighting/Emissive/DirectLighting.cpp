#include "DirectLighting.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <Scene/SceneCore.h>
#include <Core/SharedShaderResources.h>
#include <Support/Task.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::RT;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// DirectLighting
//--------------------------------------------------------------------------------------

DirectLighting::DirectLighting()
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

    // BVH
    m_rootSig.InitAsBufferSRV(2,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH);

    // emissive triangles
    m_rootSig.InitAsBufferSRV(3,
        1,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::EMISSIVE_TRIANGLE_BUFFER);

    // alias table
    m_rootSig.InitAsBufferSRV(4,
        2,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE);

    // sample set SRV
    m_rootSig.InitAsBufferSRV(5,
        3,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::PRESAMPLED_EMISSIVE_SETS,
        true);

    // mesh buffer
    m_rootSig.InitAsBufferSRV(6,
        4,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::RT_FRAME_MESH_INSTANCES);
}

DirectLighting::~DirectLighting()
{
    Reset();
}

void DirectLighting::Init()
{
    const D3D12_ROOT_SIGNATURE_FLAGS flags =
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
    RenderPassBase::InitRenderPass("DirectLighting", flags, samplers);

    TaskSet ts;

    for (int i = 0; i < (int)SHADERS::COUNT; i++)
    {
        StackStr(buff, n, "RDI_shader_%d", i);

        ts.EmplaceTask(buff, [i, this]()
            {
                m_psos[i] = m_psoLib.GetComputePSO_MT(i,
                m_rootSigObj.Get(),
                COMPILED_CS[i]);
            });
    }

    ts.Sort();
    ts.Finalize();
    App::Submit(ZetaMove(ts));

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
    CreateOutputs();

    memset(&m_cbSpatioTemporal, 0, sizeof(m_cbSpatioTemporal));
    memset(&m_cbDnsrTemporal, 0, sizeof(m_cbDnsrTemporal));
    memset(&m_cbDnsrSpatial, 0, sizeof(m_cbDnsrSpatial));
    m_cbSpatioTemporal.M_max = DefaultParamVals::M_MAX;
    m_cbSpatioTemporal.MaxRoughnessExtraBrdfSampling = 0.3;
    m_cbDnsrTemporal.MaxTsppDiffuse = m_cbDnsrSpatial.MaxTsppDiffuse = DefaultParamVals::DNSR_TSPP_DIFFUSE;
    m_cbDnsrTemporal.MaxTsppSpecular = m_cbDnsrSpatial.MaxTsppSpecular = DefaultParamVals::DNSR_TSPP_SPECULAR;
    m_cbSpatioTemporal.Denoise = m_cbDnsrTemporal.Denoise = m_cbDnsrSpatial.Denoise = true;
    m_cbDnsrSpatial.FilterDiffuse = true;
    m_cbDnsrSpatial.FilterSpecular = true;

    ParamVariant doTemporal;
    doTemporal.InitBool("Renderer", "Direct Lighting", "Temporal Resample",
        fastdelegate::MakeDelegate(this, &DirectLighting::TemporalResamplingCallback), m_doTemporalResampling);
    App::AddParam(doTemporal);

    ParamVariant doSpatial;
    doSpatial.InitBool("Renderer", "Direct Lighting", "Spatial Resample",
        fastdelegate::MakeDelegate(this, &DirectLighting::SpatialResamplingCallback), m_doSpatialResampling);
    App::AddParam(doSpatial);

    ParamVariant maxTemporalM;
    maxTemporalM.InitInt("Renderer", "Direct Lighting", "M_max",
        fastdelegate::MakeDelegate(this, &DirectLighting::MaxTemporalMCallback),
        m_cbSpatioTemporal.M_max,
        1,
        30,
        1);
    App::AddParam(maxTemporalM);

    ParamVariant denoise;
    denoise.InitBool("Renderer", "Direct Lighting", "Denoise",
        fastdelegate::MakeDelegate(this, &DirectLighting::DenoiseCallback), m_cbDnsrTemporal.Denoise);
    App::AddParam(denoise);

    ParamVariant tsppDiffuse;
    tsppDiffuse.InitInt("Renderer", "Direct Lighting", "TSPP (Diffuse)",
        fastdelegate::MakeDelegate(this, &DirectLighting::TsppDiffuseCallback),
        m_cbDnsrTemporal.MaxTsppDiffuse,            // val
        1,                                          // min
        32,                                         // max
        1);                                         // step
    App::AddParam(tsppDiffuse);

    ParamVariant tsppSpecular;
    tsppSpecular.InitInt("Renderer", "Direct Lighting", "TSPP (Specular)",
        fastdelegate::MakeDelegate(this, &DirectLighting::TsppSpecularCallback),
        m_cbDnsrTemporal.MaxTsppSpecular,            // val
        1,                                           // min
        32,                                          // max
        1);                                          // step
    App::AddParam(tsppSpecular);

    ParamVariant dnsrSpatialFilterDiffuse;
    dnsrSpatialFilterDiffuse.InitBool("Renderer", "Direct Lighting", "Spatial Filter (Diffuse)",
        fastdelegate::MakeDelegate(this, &DirectLighting::DnsrSpatialFilterDiffuseCallback), m_cbDnsrSpatial.FilterDiffuse);
    App::AddParam(dnsrSpatialFilterDiffuse);

    ParamVariant dnsrSpatialFilterSpecular;
    dnsrSpatialFilterSpecular.InitBool("Renderer", "Direct Lighting", "Spatial Filter (Specular)",
        fastdelegate::MakeDelegate(this, &DirectLighting::DnsrSpatialFilterSpecularCallback), m_cbDnsrSpatial.FilterSpecular);
    App::AddParam(dnsrSpatialFilterSpecular);

    //ParamVariant fireflyFilter;
    //fireflyFilter.InitBool("Renderer", "Direct Lighting", "Firefly Filter",
    //    fastdelegate::MakeDelegate(this, &DirectLighting::FireflyFilterCallback), m_cbDnsrTemporal.FilterFirefly);
    //App::AddParam(fireflyFilter);

    App::AddShaderReloadHandler("ReSTIR_DI", fastdelegate::MakeDelegate(this, &DirectLighting::ReloadSpatioTemporal));
    //App::AddShaderReloadHandler("ReSTIR_DI_DNSR_Temporal", fastdelegate::MakeDelegate(this, &DirectLighting::ReloadDnsrTemporal));
    //App::AddShaderReloadHandler("ReSTIR_DI_DNSR_Spatial", fastdelegate::MakeDelegate(this, &DirectLighting::ReloadDnsrSpatial));

    m_isTemporalReservoirValid = false;
}

void DirectLighting::Reset()
{
    if (IsInitialized())
    {
        for (int i = 0; i < ZetaArrayLen(m_psos); i++)
            m_psos[i] = nullptr;

        m_descTable.Reset();

        RenderPassBase::ResetRenderPass();
    }
}

void DirectLighting::OnWindowResized()
{
    CreateOutputs();
    m_isTemporalReservoirValid = false;
    m_currTemporalIdx = 0;
}

void DirectLighting::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    // resampling
    {
        Assert(!m_preSampling || (m_cbSpatioTemporal.NumSampleSets && m_cbSpatioTemporal.SampleSetSize),
            "Light presampling is enabled, but the number and size of sets haven't been set.");

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_TEMPORAL_GROUP_DIM_Y);

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI");

        computeCmdList.PIXBeginEvent("ReSTIR_DI");

        computeCmdList.SetPipelineState(m_preSampling ?
            m_psos[(int)SHADERS::SPATIO_TEMPORAL_LIGHT_PRESAMPLING] :
            m_psos[(int)SHADERS::SPATIO_TEMPORAL]);

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
        if (m_cbSpatioTemporal.Denoise)
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
        m_cbSpatioTemporal.NumGroupsInTile = RESTIR_DI_TEMPORAL_TILE_WIDTH * m_cbSpatioTemporal.DispatchDimY;
        m_cbSpatioTemporal.TemporalResampling = m_doTemporalResampling && m_isTemporalReservoirValid;
        m_cbSpatioTemporal.SpatialResampling = m_doSpatialResampling && m_isTemporalReservoirValid;

        {
            auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_A_SRV : DESC_TABLE::RESERVOIR_1_A_SRV;
            auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_B_SRV : DESC_TABLE::RESERVOIR_1_B_SRV;
            auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_UAV : DESC_TABLE::RESERVOIR_0_A_UAV;
            auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_B_UAV : DESC_TABLE::RESERVOIR_0_B_UAV;

            m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvAIdx);
            m_cbSpatioTemporal.PrevReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvBIdx);
            m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavAIdx);
            m_cbSpatioTemporal.CurrReservoir_B_DescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)uavBIdx);
        }

        m_cbSpatioTemporal.ColorAUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_A_UAV);
        m_cbSpatioTemporal.ColorBUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_UAV);
        m_cbSpatioTemporal.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);

        m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);

        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        cmdList.PIXEndEvent();
    }

    if (m_cbSpatioTemporal.Denoise)
    {
        // denoiser - temporal
        {
            computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_TEMPORAL]);

            // record the timestamp prior to execution
            const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_DNSR_Temporal");

            computeCmdList.PIXBeginEvent("ReSTIR_DI_DNSR_Temporal");

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
            m_cbDnsrTemporal.IsTemporalCacheValid = m_isDnsrTemporalCacheValid;

            m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrTemporal) / sizeof(DWORD), &m_cbDnsrTemporal);
            m_rootSig.End(computeCmdList);

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_Y);
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

            // record the timestamp after execution
            gpuTimer.EndQuery(computeCmdList, queryIdx);

            cmdList.PIXEndEvent();
        }

        // denoiser - spatial
        {
            computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DNSR_SPATIAL]);

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_Y);

            // record the timestamp prior to execution
            const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_DNSR_Spatial");

            computeCmdList.PIXBeginEvent("ReSTIR_DI_DNSR_Spatial");

            D3D12_TEXTURE_BARRIER barriers[2];

            // transition color into read state
            barriers[0] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource(),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            barriers[1] = Direct3DUtil::TextureBarrier(m_dnsrCache[m_currTemporalIdx].Specular.Resource(),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

            auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV;
            auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV;

            m_cbDnsrSpatial.TemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvDiffuseIdx);
            m_cbDnsrSpatial.TemporalCacheSpecularDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)srvSpecularIdx);
            m_cbDnsrSpatial.ColorBSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
            m_cbDnsrSpatial.FinalDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DNSR_FINAL_UAV);
            m_cbDnsrSpatial.DispatchDimX = (uint16_t)dispatchDimX;
            m_cbDnsrSpatial.DispatchDimY = (uint16_t)dispatchDimY;
            m_cbDnsrSpatial.NumGroupsInTile = RESTIR_DI_DNSR_SPATIAL_TILE_WIDTH * m_cbDnsrSpatial.DispatchDimY;

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
    m_isDnsrTemporalCacheValid = m_cbDnsrTemporal.Denoise;
}

void DirectLighting::CreateOutputs()
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

    // reservoirs
    {
        func(m_temporalReservoir[0].ReservoirA, ResourceFormats::RESERVOIR_A, "RDI_Reservoir_0_A",
            DESC_TABLE::RESERVOIR_0_A_SRV, DESC_TABLE::RESERVOIR_0_A_UAV);
        func(m_temporalReservoir[0].ReservoirB, ResourceFormats::RESERVOIR_B, "RDI_Reservoir_0_B",
            DESC_TABLE::RESERVOIR_0_B_SRV, DESC_TABLE::RESERVOIR_0_B_UAV);
        func(m_temporalReservoir[1].ReservoirA, ResourceFormats::RESERVOIR_A, "RDI_Reservoir_1_A",
            DESC_TABLE::RESERVOIR_1_A_SRV, DESC_TABLE::RESERVOIR_1_A_UAV);
        func(m_temporalReservoir[1].ReservoirB, ResourceFormats::RESERVOIR_B, "RDI_Reservoir_1_B",
            DESC_TABLE::RESERVOIR_1_B_SRV, DESC_TABLE::RESERVOIR_1_B_UAV);
    }

    {
        func(m_colorA, ResourceFormats::COLOR_A, "RDI_COLOR_A", DESC_TABLE::COLOR_A_SRV, DESC_TABLE::COLOR_A_UAV);
        func(m_colorB, ResourceFormats::COLOR_B, "RDI_COLOR_B", DESC_TABLE::COLOR_B_SRV, DESC_TABLE::COLOR_B_UAV);
    }

    // denoiser
    {
        func(m_dnsrCache[0].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Diffuse_0",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV);
        func(m_dnsrCache[1].Diffuse, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Diffuse_1",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV);
        func(m_dnsrCache[0].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Specular_0",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV);
        func(m_dnsrCache[1].Specular, ResourceFormats::DNSR_TEMPORAL_CACHE, "ReSTIR_DI_DNSR_Specular_1",
            DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV, DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV);

        m_denoised = GpuMemory::GetTexture2D("ReSTIR_DI_Denoised",
            renderer.GetRenderWidth(), renderer.GetRenderHeight(),
            ResourceFormats::DNSR_TEMPORAL_CACHE,
            D3D12_RESOURCE_STATE_COMMON,
            CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

        Direct3DUtil::CreateTexture2DUAV(m_denoised, m_descTable.CPUHandle((int)DESC_TABLE::DNSR_FINAL_UAV));
    }
}

void DirectLighting::TemporalResamplingCallback(const Support::ParamVariant& p)
{
    m_doTemporalResampling = p.GetBool();
}

void DirectLighting::SpatialResamplingCallback(const Support::ParamVariant& p)
{
    m_doSpatialResampling = p.GetBool();
}

void DirectLighting::MaxTemporalMCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.M_max = (uint16_t) p.GetInt().m_value;
}

void DirectLighting::MaxRoughessExtraBrdfSamplingCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.MaxRoughnessExtraBrdfSampling = Math::Max(p.GetFloat().m_value, 
        m_cbSpatioTemporal.MaxRoughnessExtraBrdfSampling);
}

void DirectLighting::DenoiseCallback(const Support::ParamVariant& p)
{
    m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
    m_cbSpatioTemporal.Denoise = p.GetBool();
    m_cbDnsrTemporal.Denoise = p.GetBool();
    m_cbDnsrSpatial.Denoise = p.GetBool();
    m_isDnsrTemporalCacheValid = !m_cbDnsrTemporal.Denoise ? false : m_isDnsrTemporalCacheValid;
}

void DirectLighting::TsppDiffuseCallback(const Support::ParamVariant& p)
{
    m_cbDnsrTemporal.MaxTsppDiffuse = (uint16_t)p.GetInt().m_value;
    m_cbDnsrSpatial.MaxTsppDiffuse = (uint16_t)p.GetInt().m_value;
}

void DirectLighting::TsppSpecularCallback(const Support::ParamVariant& p)
{
    m_cbDnsrTemporal.MaxTsppSpecular = (uint16_t)p.GetInt().m_value;
    m_cbDnsrSpatial.MaxTsppSpecular = (uint16_t)p.GetInt().m_value;
}

void DirectLighting::DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p)
{
    m_cbDnsrSpatial.FilterDiffuse = p.GetBool();
}

void DirectLighting::DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p)
{
    m_cbDnsrSpatial.FilterSpecular = p.GetBool();
}

void DirectLighting::ReloadSpatioTemporal()
{
    const int i = m_preSampling ? (int)SHADERS::SPATIO_TEMPORAL_LIGHT_PRESAMPLING :
        (int)SHADERS::SPATIO_TEMPORAL;

    m_psoLib.Reload(i, m_preSampling ?
        "DirectLighting\\Emissive\\ReSTIR_DI_SpatioTemporal_LP.hlsl" :
        "DirectLighting\\Emissive\\ReSTIR_DI_SpatioTemporal.hlsl",
        true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void DirectLighting::ReloadDnsrTemporal()
{
    const int i = (int)SHADERS::DNSR_TEMPORAL;

    m_psoLib.Reload(i, "DirectLighting\\Emissive\\ReSTIR_DI_DNSR_Temporal.hlsl", true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void DirectLighting::ReloadDnsrSpatial()
{
    const int i = (int)SHADERS::DNSR_SPATIAL;

    m_psoLib.Reload(i, "DirectLighting\\Emissive\\ReSTIR_DI_DNSR_Spatial.hlsl", true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}
