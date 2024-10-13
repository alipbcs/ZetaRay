#include "SkyDI.h"
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <Support/Task.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Core::Direct3DUtil;
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
    m_rootSig.InitAsConstants(0, NUM_CONSTS, 1);

    // frame constants
    m_rootSig.InitAsCBV(1, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // BVH
    m_rootSig.InitAsBufferSRV(2, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH);
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

    for (int i = 0; i < (int)SHADER::COUNT; i++)
    {
        StackStr(buff, n, "SkyDI_shader_%d", i);

        ts.EmplaceTask(buff, [i, this]()
            {
                m_psoLib.CompileComputePSO_MT(i, m_rootSigObj.Get(),
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
    m_cbSpatioTemporal.M_max = DefaultParamVals::M_MAX_SKY | (DefaultParamVals::M_MAX_SUN << 16);
    m_cbSpatioTemporal.Alpha_min = DefaultParamVals::ROUGHNESS_MIN * DefaultParamVals::ROUGHNESS_MIN;
    m_cbDnsrTemporal.MaxTsppDiffuse = m_cbDnsrSpatial.MaxTsppDiffuse = DefaultParamVals::DNSRTspp_Diffuse;
    m_cbDnsrTemporal.MaxTsppSpecular = m_cbDnsrSpatial.MaxTsppSpecular = DefaultParamVals::DNSRTspp_Specular;
    // SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE, true);
    // SET_CB_FLAG(m_cbDnsrTemporal, CB_SKY_DI_DNSR_TEMPORAL_FLAGS::DENOISE, true);
    // SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::DENOISE, true);
    // SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_DIFFUSE, true);
    // SET_CB_FLAG(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_SPECULAR, true);

    ParamVariant doTemporal;
    doTemporal.InitBool("Renderer", "Direct Lighting", "Temporal Resample",
        fastdelegate::MakeDelegate(this, &SkyDI::TemporalResamplingCallback), m_temporalResampling);
    App::AddParam(doTemporal);

    ParamVariant doSpatial;
    doSpatial.InitBool("Renderer", "Direct Lighting", "Spatial Resample",
        fastdelegate::MakeDelegate(this, &SkyDI::SpatialResamplingCallback), m_spatialResampling);
    App::AddParam(doSpatial);

    ParamVariant m_max_sky;
    m_max_sky.InitInt("Renderer", "Direct Lighting", "M_max (Sky)",
        fastdelegate::MakeDelegate(this, &SkyDI::MaxMSkyCallback),
        DefaultParamVals::M_MAX_SKY, 1, 15, 1);
    App::AddParam(m_max_sky);

    ParamVariant m_max_sun;
    m_max_sun.InitInt("Renderer", "Direct Lighting", "M_max (Sun)",
        fastdelegate::MakeDelegate(this, &SkyDI::MaxMSunCallback),
        DefaultParamVals::M_MAX_SUN, 1, 15, 1);
    App::AddParam(m_max_sun);

    ParamVariant alphaMin;
    alphaMin.InitFloat("Renderer", "Direct Lighting", "Alpha_min",
        fastdelegate::MakeDelegate(this, &SkyDI::AlphaMinCallback),
        DefaultParamVals::ROUGHNESS_MIN, 0.0f, 1.0f, 1e-2f);
    App::AddParam(alphaMin);

    // ParamVariant denoise;
    // denoise.InitBool("Renderer", "Direct Lighting", "Denoise",
    //     fastdelegate::MakeDelegate(this, &SkyDI::DenoisingCallback), 
    //     IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE));
    // App::AddParam(denoise);

    // ParamVariant tsppDiffuse;
    // tsppDiffuse.InitInt("Renderer", "Direct Lighting", "TSPP (Diffuse)",
    //     fastdelegate::MakeDelegate(this, &SkyDI::TsppDiffuseCallback),
    //     m_cbDnsrTemporal.MaxTsppDiffuse, 1, 32, 1);
    // App::AddParam(tsppDiffuse);

    // ParamVariant tsppSpecular;
    // tsppSpecular.InitInt("Renderer", "Direct Lighting", "TSPP (Specular)",
    //     fastdelegate::MakeDelegate(this, &SkyDI::TsppSpecularCallback),
    //     m_cbDnsrTemporal.MaxTsppSpecular, 1, 32, 1);
    // App::AddParam(tsppSpecular);

    // ParamVariant dnsrSpatialFilterDiffuse;
    // dnsrSpatialFilterDiffuse.InitBool("Renderer", "Direct Lighting", "Spatial Filter (Diffuse)",
    //     fastdelegate::MakeDelegate(this, &SkyDI::DnsrSpatialFilterDiffuseCallback), 
    //     IS_CB_FLAG_SET(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_DIFFUSE));
    // App::AddParam(dnsrSpatialFilterDiffuse);

    // ParamVariant dnsrSpatialFilterSpecular;
    // dnsrSpatialFilterSpecular.InitBool("Renderer", "Direct Lighting", "Spatial Filter (Specular)",
    //     fastdelegate::MakeDelegate(this, &SkyDI::DnsrSpatialFilterSpecularCallback), 
    //     IS_CB_FLAG_SET(m_cbDnsrSpatial, CB_SKY_DI_DNSR_SPATIAL_FLAGS::FILTER_SPECULAR));
    // App::AddParam(dnsrSpatialFilterSpecular);

    App::AddShaderReloadHandler("SkyDI", fastdelegate::MakeDelegate(this, &SkyDI::ReloadTemporalPass));
    //App::AddShaderReloadHandler("SkyDI_DNSR_Temporal", fastdelegate::MakeDelegate(this, &SkyDI::ReloadDNSRTemporal));
    //App::AddShaderReloadHandler("SkyDI_DNSR_Spatial", fastdelegate::MakeDelegate(this, &SkyDI::ReloadDNSRSpatial));

    m_isTemporalReservoirValid = false;

    waitObj.Wait();
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

    // Sky DI
    {
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, SKY_DI_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, SKY_DI_GROUP_DIM_Y);

        computeCmdList.PIXBeginEvent("SkyDI");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI");

        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, Reservoir::NUM * 2> barriers;

        // transition current temporal reservoir into write state
        barriers.push_back(TextureBarrier_SrvToUavNoSync(
            m_reservoir[m_currTemporalIdx].A.Resource()));
        barriers.push_back(TextureBarrier_SrvToUavNoSync(
            m_reservoir[m_currTemporalIdx].B.Resource()));
        barriers.push_back(TextureBarrier_SrvToUavNoSync(
            m_reservoir[m_currTemporalIdx].C.Resource()));

        // transition color outputs into write state
        if (IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE))
        {
            barriers.push_back(TextureBarrier_SrvToUavNoSync(m_colorA.Resource()));
            barriers.push_back(TextureBarrier_SrvToUavNoSync(m_colorB.Resource()));
        }

        // transition previous reservoirs into read state
        if (m_isTemporalReservoirValid)
        {
            barriers.push_back(TextureBarrier_UavToSrvNoSync(
                m_reservoir[1 - m_currTemporalIdx].A.Resource()));
            barriers.push_back(TextureBarrier_UavToSrvNoSync(
                m_reservoir[1 - m_currTemporalIdx].B.Resource()));
            barriers.push_back(TextureBarrier_UavToSrvNoSync(
                m_reservoir[1 - m_currTemporalIdx].C.Resource()));
        }

        computeCmdList.ResourceBarrier(barriers.data(), (UINT)barriers.size());

        m_cbSpatioTemporal.DispatchDimX = (uint16_t)dispatchDimX;
        m_cbSpatioTemporal.DispatchDimY = (uint16_t)dispatchDimY;
        m_cbSpatioTemporal.NumGroupsInTile = SKY_DI_TILE_WIDTH * m_cbSpatioTemporal.DispatchDimY;
        SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::TEMPORAL_RESAMPLE, 
            m_temporalResampling && m_isTemporalReservoirValid);
        SET_CB_FLAG(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::SPATIAL_RESAMPLE, 
            m_spatialResampling && m_isTemporalReservoirValid);

        auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_A_SRV : 
            DESC_TABLE::RESERVOIR_1_A_SRV;
        auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_UAV : 
            DESC_TABLE::RESERVOIR_0_A_UAV;

        m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvAIdx);
        m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavAIdx);
        // m_cbSpatioTemporal.ColorAUavDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::COLOR_A_UAV);
        // m_cbSpatioTemporal.ColorBUavDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::COLOR_B_UAV);
        m_cbSpatioTemporal.FinalDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::FINAL_UAV);;

        m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::SKY_DI));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        cmdList.PIXEndEvent();
    }

    // denoiser
    if(IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_SKY_DI_FLAGS::DENOISE))
    {
        // denoiser - temporal
        {
            computeCmdList.PIXBeginEvent("SkyDI_DNSR_Temporal");
            const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_DNSR_Temporal");

            D3D12_TEXTURE_BARRIER barriers[4];

            // transition color into read state
            barriers[0] = TextureBarrier_UavToSrvWithSync(m_colorA.Resource());
            barriers[1] = TextureBarrier_UavToSrvWithSync(m_colorB.Resource());
            // transition current denoiser caches into write
            barriers[2] = TextureBarrier_SrvToUavNoSync(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource());
            barriers[3] = TextureBarrier_SrvToUavNoSync(m_dnsrCache[m_currTemporalIdx].Specular.Resource());

            computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

            auto srvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV;
            auto srvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV;
            auto uavDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV;
            auto uavSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV :
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV;

            m_cbDnsrTemporal.ColorASrvDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::COLOR_A_SRV);
            m_cbDnsrTemporal.ColorBSrvDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
            m_cbDnsrTemporal.PrevTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvDiffuseIdx);
            m_cbDnsrTemporal.PrevTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvSpecularIdx);
            m_cbDnsrTemporal.CurrTemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavDiffuseIdx);
            m_cbDnsrTemporal.CurrTemporalCacheSpecularDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavSpecularIdx);
            SET_CB_FLAG(m_cbDnsrTemporal, CB_SKY_DI_DNSR_TEMPORAL_FLAGS::CACHE_VALID, m_isDnsrTemporalCacheValid);

            m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrTemporal) / sizeof(DWORD), &m_cbDnsrTemporal);
            m_rootSig.End(computeCmdList);

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, SKY_DI_DNSR_TEMPORAL_GROUP_DIM_Y);
            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::DNSR_TEMPORAL));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

            gpuTimer.EndQuery(computeCmdList, queryIdx);
            cmdList.PIXEndEvent();
        }

        // denoiser - spatial
        {
            computeCmdList.PIXBeginEvent("SkyDI_DNSR_Spatial");
            const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyDI_DNSR_Spatial");

            D3D12_TEXTURE_BARRIER spatialBarriers[2];

            // transition color into read state
            spatialBarriers[0] = TextureBarrier_UavToSrvWithSync(m_dnsrCache[m_currTemporalIdx].Diffuse.Resource());
            spatialBarriers[1] = TextureBarrier_UavToSrvWithSync(m_dnsrCache[m_currTemporalIdx].Specular.Resource());

            computeCmdList.ResourceBarrier(spatialBarriers, ZetaArrayLen(spatialBarriers));

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, SKY_DI_DNSR_SPATIAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y);

            auto spatialSrvDiffuseIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV : 
                DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV;
            auto spatialSrvSpecularIdx = m_currTemporalIdx == 1 ? DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV : 
                DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV;

            m_cbDnsrSpatial.TemporalCacheDiffuseDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)spatialSrvDiffuseIdx);
            m_cbDnsrSpatial.TemporalCacheSpecularDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)spatialSrvSpecularIdx);
            m_cbDnsrSpatial.ColorBSrvDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::COLOR_B_SRV);
            m_cbDnsrSpatial.FinalDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::FINAL_UAV);
            m_cbDnsrSpatial.DispatchDimX = (uint16_t)dispatchDimX;
            m_cbDnsrSpatial.DispatchDimY = (uint16_t)dispatchDimY;
            m_cbDnsrSpatial.NumGroupsInTile = SKY_DI_DNSR_SPATIAL_TILE_WIDTH * m_cbDnsrSpatial.DispatchDimY;

            m_rootSig.SetRootConstants(0, sizeof(m_cbDnsrSpatial) / sizeof(DWORD), &m_cbDnsrSpatial);
            m_rootSig.End(computeCmdList);

            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::DNSR_SPATIAL));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

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
    const auto w = renderer.GetRenderWidth();
    const auto h = renderer.GetRenderHeight();

    constexpr int N = 2 * Reservoir::NUM + 1;
    PlacedResourceList<N> list;

    // reservoirs
    for (int i = 0; i < 2; i++)
    {
        list.PushTex2D(ResourceFormats::RESERVOIR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats::RESERVOIR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats::RESERVOIR_C, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    // final
    list.PushTex2D(ResourceFormats::FINAL, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    // // denoiser
    // list.PushTex2D(ResourceFormats::COLOR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    // list.PushTex2D(ResourceFormats::COLOR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    // for (int i = 0; i < 2; i++)
    // {
    //     list.PushTex2D(ResourceFormats::FINAL, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    //     list.PushTex2D(ResourceFormats::FINAL, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    // }

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

    static_assert((int)(DESC_TABLE::RESERVOIR_0_A_SRV) + 1 == (int)DESC_TABLE::RESERVOIR_0_B_SRV);
    static_assert((int)(DESC_TABLE::RESERVOIR_0_A_SRV) + 2 == (int)DESC_TABLE::RESERVOIR_0_C_SRV);
    static_assert((int)(DESC_TABLE::RESERVOIR_0_A_UAV) + 1 == (int)DESC_TABLE::RESERVOIR_0_B_UAV);
    static_assert((int)(DESC_TABLE::RESERVOIR_0_A_UAV) + 2 == (int)DESC_TABLE::RESERVOIR_0_C_UAV);
    static_assert((int)(DESC_TABLE::RESERVOIR_1_A_SRV) + 1 == (int)DESC_TABLE::RESERVOIR_1_B_SRV);
    static_assert((int)(DESC_TABLE::RESERVOIR_1_A_SRV) + 2 == (int)DESC_TABLE::RESERVOIR_1_C_SRV);
    static_assert((int)(DESC_TABLE::RESERVOIR_1_A_UAV) + 1 == (int)DESC_TABLE::RESERVOIR_1_B_UAV);
    static_assert((int)(DESC_TABLE::RESERVOIR_1_A_UAV) + 2 == (int)DESC_TABLE::RESERVOIR_1_C_UAV);

    for (int i = 0; i < 2; i++)
    {
        const auto state = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
        const int descOffset = i * Reservoir::NUM * 2;

        func(m_reservoir[i].A, ResourceFormats::RESERVOIR_A,
            "SkyDI_Reservoir", i, "A", allocs[currRes++],
            (int)DESC_TABLE::RESERVOIR_0_A_SRV, (int)DESC_TABLE::RESERVOIR_0_A_UAV,
            descOffset, state);
        func(m_reservoir[i].B, ResourceFormats::RESERVOIR_B,
            "SkyDI_Reservoir", i, "B", allocs[currRes++],
            (int)DESC_TABLE::RESERVOIR_0_B_SRV, (int)DESC_TABLE::RESERVOIR_0_B_UAV,
            descOffset, state);
        func(m_reservoir[i].C, ResourceFormats::RESERVOIR_C,
            "SkyDI_Reservoir", i, "C", allocs[currRes++],
            (int)DESC_TABLE::RESERVOIR_0_C_SRV, (int)DESC_TABLE::RESERVOIR_0_C_UAV,
            descOffset, state);
    }

    m_final = GpuMemory::GetPlacedTexture2D("SkyDI_final", w, h, ResourceFormats::FINAL, 
        m_resHeap.Heap(), allocs[currRes++].Offset,
        D3D12_RESOURCE_STATE_COMMON,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE::FINAL_UAV));

    {
        // func(m_colorA, ResourceFormats::COLOR_A, 
        //     "SkyDI_COLOR_A", 0, "", allocs[currRes++], 
        //     (int)DESC_TABLE::COLOR_A_SRV, (int)DESC_TABLE::COLOR_A_UAV,
        //     0, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);
        // func(m_colorB, ResourceFormats::COLOR_B, 
        //     "SkyDI_COLOR_B", 0, "", allocs[currRes++], 
        //     (int)DESC_TABLE::COLOR_B_SRV, (int)DESC_TABLE::COLOR_B_UAV,
        //     0, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);
    }

    // denoiser cache
    // for (int i = 0; i < 2; i++)
    // {
    //     const auto state = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
    //     const int descOffset = 4;

    //     func(m_dnsrCache[i].Diffuse, ResourceFormats::FINAL, 
    //         "SkyDI_DNSR", i, "Diffuse", allocs[currRes++], 
    //         (int)DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV, (int)DESC_TABLE::DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV,
    //         descOffset, state);
    //     func(m_dnsrCache[i].Specular, ResourceFormats::FINAL, 
    //         "SkyDI_DNSR", i, "Specular", allocs[currRes++], 
    //         (int)DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV, (int)DESC_TABLE::DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV,
    //         descOffset, state);
    // }
}

void SkyDI::TemporalResamplingCallback(const Support::ParamVariant& p)
{
    m_temporalResampling = p.GetBool();
}

void SkyDI::SpatialResamplingCallback(const Support::ParamVariant& p)
{
    m_spatialResampling = p.GetBool();
}

void SkyDI::MaxMSkyCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.M_max = (m_cbSpatioTemporal.M_max & 0xffff0000) | p.GetInt().m_value;
}

void SkyDI::MaxMSunCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.M_max = (m_cbSpatioTemporal.M_max & 0xffff) | (p.GetInt().m_value << 16);
}

void SkyDI::AlphaMinCallback(const Support::ParamVariant& p)
{
    float newVal = p.GetFloat().m_value;
    m_cbSpatioTemporal.Alpha_min = newVal * newVal;
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

void SkyDI::ReloadTemporalPass()
{
    const int i = (int)SHADER::SKY_DI;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "DirectLighting\\Sky\\SkyDI.hlsl");
}

void SkyDI::ReloadDNSRTemporal()
{
    const int i = (int)SHADER::DNSR_TEMPORAL;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "DirectLighting\\Sky\\SkyDI_DNSR_Temporal.hlsl");
}

void SkyDI::ReloadDNSRSpatial()
{
    const int i = (int)SHADER::DNSR_SPATIAL;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "DirectLighting\\Sky\\SkyDI_DNSR_Spatial.hlsl");
}
