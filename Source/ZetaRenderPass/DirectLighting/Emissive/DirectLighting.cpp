#include "DirectLighting.h"
#include <Core/CommandList.h>
#include <Scene/SceneCore.h>
#include <Support/Param.h>
#include <Support/Task.h>
#include "../Assets/Font/IconsFontAwesome6.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Core::Direct3DUtil;
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
    m_rootSig.InitAsCBV(0, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1, NUM_CONSTS, 1);

    // BVH
    m_rootSig.InitAsBufferSRV(2, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH_CURR);
    // BVH
    m_rootSig.InitAsBufferSRV(3, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH_PREV);

    // emissive triangles
    m_rootSig.InitAsBufferSRV(4, 2, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::EMISSIVE_TRIANGLE_BUFFER);

    // alias table
    m_rootSig.InitAsBufferSRV(5, 3, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE);

    // sample set SRV
    m_rootSig.InitAsBufferSRV(6, 4, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::PRESAMPLED_EMISSIVE_SETS,
        true);

    // mesh buffer
    m_rootSig.InitAsBufferSRV(7, 5, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);
}

void DirectLighting::Init()
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
    RenderPassBase::InitRenderPass("DirectLighting", flags, samplers);

    TaskSet ts;

    for (int i = 0; i < (int)SHADER::COUNT; i++)
    {
        StackStr(buff, n, "RDI_shader_%d", i);

        ts.EmplaceTask(buff, [i, this]()
            {
                m_psoLib.CompileComputePSO_MT(i, m_rootSigObj.Get(),
                    COMPILED_CS[i]);
            });
    }

    ts.Sort();
    ts.Finalize();
    App::Submit(ZetaMove(ts));

    memset(&m_cbSpatioTemporal, 0, sizeof(m_cbSpatioTemporal));
    m_cbSpatioTemporal.M_max = DefaultParamVals::M_MAX;
    m_cbSpatioTemporal.Alpha_min = DefaultParamVals::ROUGHNESS_MIN * DefaultParamVals::ROUGHNESS_MIN;
    SET_CB_FLAG(m_cbSpatioTemporal, CB_RDI_FLAGS::STOCHASTIC_SPATIAL, true);
    SET_CB_FLAG(m_cbSpatioTemporal, CB_RDI_FLAGS::EXTRA_DISOCCLUSION_SAMPLING, true);

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
    CreateOutputs();

    ParamVariant doTemporal;
    doTemporal.InitBool(ICON_FA_FILM " Renderer", "Direct Lighting", "Temporal Resample",
        fastdelegate::MakeDelegate(this, &DirectLighting::TemporalResamplingCallback), m_temporalResampling);
    App::AddParam(doTemporal);

    ParamVariant doSpatial;
    doSpatial.InitBool(ICON_FA_FILM " Renderer", "Direct Lighting", "Spatial Resample",
        fastdelegate::MakeDelegate(this, &DirectLighting::SpatialResamplingCallback), m_spatialResampling);
    App::AddParam(doSpatial);

    ParamVariant maxTemporalM;
    maxTemporalM.InitInt(ICON_FA_FILM " Renderer", "Direct Lighting", "M_max",
        fastdelegate::MakeDelegate(this, &DirectLighting::MaxTemporalMCallback),
        m_cbSpatioTemporal.M_max, 1, 30, 1);
    App::AddParam(maxTemporalM);

    ParamVariant extraDissocclusion;
    extraDissocclusion.InitBool(ICON_FA_FILM " Renderer", "Direct Lighting", "Extra Sampling (Disocclusion)",
        fastdelegate::MakeDelegate(this, &DirectLighting::ExtraSamplesDisocclusionCallback), 
        IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_RDI_FLAGS::EXTRA_DISOCCLUSION_SAMPLING));
    App::AddParam(extraDissocclusion);

    ParamVariant stochasticSpatial;
    stochasticSpatial.InitBool(ICON_FA_FILM " Renderer", "Direct Lighting", "Stochastic Spatial",
        fastdelegate::MakeDelegate(this, &DirectLighting::StochasticSpatialCallback), 
        IS_CB_FLAG_SET(m_cbSpatioTemporal, CB_RDI_FLAGS::STOCHASTIC_SPATIAL));
    App::AddParam(stochasticSpatial);

    ParamVariant alphaMin;
    alphaMin.InitFloat(ICON_FA_FILM " Renderer", "Direct Lighting", "Alpha_min",
        fastdelegate::MakeDelegate(this, &DirectLighting::AlphaMinCallback),
        DefaultParamVals::ROUGHNESS_MIN, 0.0f, 1.0f, 1e-2f);
    App::AddParam(alphaMin);

    App::AddShaderReloadHandler("ReSTIR_DI", fastdelegate::MakeDelegate(this, 
        &DirectLighting::ReloadTemporal));

    m_isTemporalReservoirValid = false;
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

    Assert(!m_preSampling || (m_cbSpatioTemporal.NumSampleSets && m_cbSpatioTemporal.SampleSetSize),
        "Light presampling is enabled, but the number and size of sets haven't been set.");

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_DI_TEMPORAL_GROUP_DIM_X);
    const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_DI_TEMPORAL_GROUP_DIM_Y);

    const bool doTemporal = m_isTemporalReservoirValid && m_temporalResampling;
    const bool doSpatial = doTemporal && m_spatialResampling;

    SET_CB_FLAG(m_cbSpatioTemporal, CB_RDI_FLAGS::TEMPORAL_RESAMPLE, doTemporal);
    SET_CB_FLAG(m_cbSpatioTemporal, CB_RDI_FLAGS::SPATIAL_RESAMPLE, doSpatial);

    m_cbSpatioTemporal.DispatchDimX = (uint16_t)dispatchDimX;
    m_cbSpatioTemporal.DispatchDimY = (uint16_t)dispatchDimY;
    m_cbSpatioTemporal.NumGroupsInTile = RESTIR_DI_TILE_WIDTH * m_cbSpatioTemporal.DispatchDimY;

    // Initial candidates and temporal
    {
        computeCmdList.PIXBeginEvent("ReSTIR_DI_Temporal");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_Temporal");

        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, Reservoir::NUM * 2> barriers;

        // Current reservoirs into UAV
        if (m_reservoir[m_currTemporalIdx].Layout != D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS)
        {
            barriers.push_back(TextureBarrier_SrvToUavNoSync(
                m_reservoir[m_currTemporalIdx].A.Resource()));
            barriers.push_back(TextureBarrier_SrvToUavNoSync(
                m_reservoir[m_currTemporalIdx].B.Resource()));

            m_reservoir[m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;
        }

        // Temporal reservoirs into SRV
        if (doTemporal)
        {
            if (m_reservoir[1 - m_currTemporalIdx].Layout ==
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS)
            {
                barriers.push_back(TextureBarrier_UavToSrvNoSync(
                    m_reservoir[1 - m_currTemporalIdx].A.Resource()));
                barriers.push_back(TextureBarrier_UavToSrvNoSync(
                    m_reservoir[1 - m_currTemporalIdx].B.Resource()));

                m_reservoir[1 - m_currTemporalIdx].Layout =
                    D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
            }
        }

        if (!barriers.empty())
            computeCmdList.ResourceBarrier(barriers.data(), (UINT)barriers.size());

        auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_0_A_SRV :
            DESC_TABLE::RESERVOIR_1_A_SRV;
        auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_UAV :
            DESC_TABLE::RESERVOIR_0_A_UAV;

        m_cbSpatioTemporal.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvAIdx);
        m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavAIdx);

        m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);
        m_rootSig.End(computeCmdList);

        auto sh = m_preSampling ? SHADER::TEMPORAL_LIGHT_PRESAMPLING : SHADER::TEMPORAL;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        cmdList.PIXEndEvent();
    }

    // Spatial
    if (doSpatial)
    {
        computeCmdList.PIXBeginEvent("ReSTIR_DI_Spatial");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_DI_Spatial");

        D3D12_TEXTURE_BARRIER barriers[Reservoir::NUM];

        // current reservoirs into UAV
        barriers[0] = TextureBarrier_UavToSrvWithSync(m_reservoir[m_currTemporalIdx].A.Resource());
        barriers[1] = TextureBarrier_UavToSrvWithSync(m_reservoir[m_currTemporalIdx].B.Resource());
        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

        m_reservoir[m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;

        auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE::RESERVOIR_1_A_SRV :
            DESC_TABLE::RESERVOIR_0_A_SRV;

        m_cbSpatioTemporal.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvAIdx);

        m_rootSig.SetRootConstants(0, sizeof(m_cbSpatioTemporal) / sizeof(DWORD), &m_cbSpatioTemporal);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::SPATIAL));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        cmdList.PIXEndEvent();
    }

    m_isTemporalReservoirValid = true;
    m_currTemporalIdx = 1 - m_currTemporalIdx;
}

void DirectLighting::CreateOutputs()
{
    auto& renderer = App::GetRenderer();
    const auto w = renderer.GetRenderWidth();
    const auto h = renderer.GetRenderHeight();

    constexpr int N = 2 * Reservoir::NUM + 1 + 1;
    PlacedResourceList<N> list;

    // reservoirs
    for (int i = 0; i < 2; i++)
    {
        list.PushTex2D(ResourceFormats::RESERVOIR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats::RESERVOIR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    // target
    list.PushTex2D(ResourceFormats::TARGET, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    // final
    list.PushTex2D(ResourceFormats::FINAL, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    list.End();

    m_resHeap = GpuMemory::GetResourceHeap(list.TotalSizeInBytes());
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
    static_assert((int)(DESC_TABLE::RESERVOIR_0_A_UAV) + 1 == (int)DESC_TABLE::RESERVOIR_0_B_UAV);
    static_assert((int)(DESC_TABLE::RESERVOIR_1_A_SRV) + 1 == (int)DESC_TABLE::RESERVOIR_1_B_SRV);
    static_assert((int)(DESC_TABLE::RESERVOIR_1_A_UAV) + 1 == (int)DESC_TABLE::RESERVOIR_1_B_UAV);

    const D3D12_BARRIER_LAYOUT initLayout[2] = { D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
        m_temporalResampling ?
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE :
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS };

    // reservoirs
    for (int i = 0; i < 2; i++)
    {
        const int descOffset = i * Reservoir::NUM * 2;
        const auto layout = initLayout[i];

        func(m_reservoir[i].A, ResourceFormats::RESERVOIR_A,
            "RDI_Reservoir", i, "A", allocs[currRes++],
            (int)DESC_TABLE::RESERVOIR_0_A_SRV, (int)DESC_TABLE::RESERVOIR_0_A_UAV,
            descOffset, layout);
        func(m_reservoir[i].B, ResourceFormats::RESERVOIR_B,
            "RDI_Reservoir", i, "B", allocs[currRes++],
            (int)DESC_TABLE::RESERVOIR_0_B_SRV, (int)DESC_TABLE::RESERVOIR_0_B_UAV,
            descOffset, layout);

        m_reservoir[i].Layout = initLayout[i];
    }

    // target
    m_target = GpuMemory::GetPlacedTexture2D("RDI_target", w, h, ResourceFormats::TARGET,
        m_resHeap.Heap(), allocs[currRes++].Offset,
        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    // final
    m_final = GpuMemory::GetPlacedTexture2D("RDI_Final", w, h, ResourceFormats::FINAL,
        m_resHeap.Heap(), allocs[currRes++].Offset,
        D3D12_RESOURCE_STATE_COMMON,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    Direct3DUtil::CreateTexture2DUAV(m_target, m_descTable.CPUHandle((int)DESC_TABLE::TARGET_UAV));
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE::FINAL_UAV));

    // Following never change, so can be set only once
    m_cbSpatioTemporal.TargetDescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE::TARGET_UAV);
    m_cbSpatioTemporal.FinalDescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE::FINAL_UAV);
}

void DirectLighting::TemporalResamplingCallback(const Support::ParamVariant& p)
{
    m_temporalResampling = p.GetBool();
    App::GetScene().SceneModified();
}

void DirectLighting::SpatialResamplingCallback(const Support::ParamVariant& p)
{
    m_spatialResampling = p.GetBool();
    App::GetScene().SceneModified();
}

void DirectLighting::MaxTemporalMCallback(const Support::ParamVariant& p)
{
    m_cbSpatioTemporal.M_max = (uint16_t)p.GetInt().m_value;
    App::GetScene().SceneModified();
}

void DirectLighting::ExtraSamplesDisocclusionCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbSpatioTemporal, CB_RDI_FLAGS::EXTRA_DISOCCLUSION_SAMPLING, p.GetBool());
    App::GetScene().SceneModified();
}

void DirectLighting::StochasticSpatialCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbSpatioTemporal, CB_RDI_FLAGS::STOCHASTIC_SPATIAL, p.GetBool());
    App::GetScene().SceneModified();
}

void DirectLighting::AlphaMinCallback(const Support::ParamVariant& p)
{
    float newVal = p.GetFloat().m_value;
    m_cbSpatioTemporal.Alpha_min = newVal * newVal;

    App::GetScene().SceneModified();
}

void DirectLighting::ReloadTemporal()
{
    const int i = m_preSampling ? (int)SHADER::TEMPORAL_LIGHT_PRESAMPLING :
        (int)SHADER::TEMPORAL;

    m_psoLib.Reload(i, m_rootSigObj.Get(), m_preSampling ?
        "DirectLighting\\Emissive\\ReSTIR_DI_Temporal_WPS.hlsl" :
        "DirectLighting\\Emissive\\ReSTIR_DI_Temporal.hlsl");
}
