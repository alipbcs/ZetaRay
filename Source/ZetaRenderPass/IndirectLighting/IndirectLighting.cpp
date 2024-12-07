#include "IndirectLighting.h"
#include <Core/CommandList.h>
#include <Core/SharedShaderResources.h>
#include <Support/Param.h>
#include <Scene/SceneCore.h>
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
using namespace ZetaRay::Util;

namespace
{
    constexpr int EnumToSamplerIdx(TEXTURE_FILTER f)
    {
        if (f == TEXTURE_FILTER::MIP0)
            return 0;
        if (f == TEXTURE_FILTER::TRI_LINEAR)
            return 3;
        if (f == TEXTURE_FILTER::ANISOTROPIC_2X)
            return 6;
        if (f == TEXTURE_FILTER::ANISOTROPIC_4X)
            return 7;

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
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC);

    // mesh buffer
    m_rootSig.InitAsBufferSRV(3, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);

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

    // light voxel grid/path state
    m_rootSig.InitAsBufferSRV(10, 8, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        nullptr,
        true);
}

void IndirectLighting::InitPSOs()
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
}

void IndirectLighting::Init(INTEGRATOR method)
{
    InitPSOs();

    memset(&m_cbRGI, 0, sizeof(m_cbRGI));
    memset(&m_cbRPT_PathTrace, 0, sizeof(m_cbRPT_PathTrace));
    memset(&m_cbRPT_Reuse, 0, sizeof(m_cbRPT_Reuse));
    m_cbRGI.M_max = DefaultParamVals::M_MAX;
    m_cbRPT_PathTrace.Alpha_min = m_cbRPT_Reuse.Alpha_min =
        DefaultParamVals::ROUGHNESS_MIN * DefaultParamVals::ROUGHNESS_MIN;
    m_cbRGI.MaxNonTrBounces = DefaultParamVals::MAX_NON_TR_BOUNCES;
    m_cbRGI.MaxGlossyTrBounces = DefaultParamVals::MAX_GLOSSY_TR_BOUNCES;
    m_cbRPT_PathTrace.TexFilterDescHeapIdx = EnumToSamplerIdx(DefaultParamVals::TEX_FILTER);
    m_cbRPT_PathTrace.Packed = m_cbRPT_Reuse.Packed = DefaultParamVals::MAX_NON_TR_BOUNCES |
        (DefaultParamVals::MAX_GLOSSY_TR_BOUNCES << PACKED_INDEX::NUM_GLOSSY_BOUNCES) |
        (DefaultParamVals::M_MAX << PACKED_INDEX::MAX_TEMPORAL_M) |
        (DefaultParamVals::M_MAX_SPATIAL << PACKED_INDEX::MAX_SPATIAL_M) |
        (m_cbRPT_PathTrace.TexFilterDescHeapIdx << PACKED_INDEX::TEX_FILTER);
    m_cbRGI.TexFilterDescHeapIdx = m_cbRPT_PathTrace.TexFilterDescHeapIdx;

    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::RUSSIAN_ROULETTE, DefaultParamVals::RUSSIAN_ROULETTE);
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::RUSSIAN_ROULETTE, DefaultParamVals::RUSSIAN_ROULETTE);
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::RUSSIAN_ROULETTE, DefaultParamVals::RUSSIAN_ROULETTE);
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::SORT_TEMPORAL, true);
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_TEMPORAL, true);
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_SPATIAL, true);
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::BOILING_SUPPRESSION, DefaultParamVals::BOILING_SUPPRESSION);

    ParamVariant rr;
    rr.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Russian Roulette",
        fastdelegate::MakeDelegate(this, &IndirectLighting::RussianRouletteCallback),
        DefaultParamVals::RUSSIAN_ROULETTE, "Path Sampling");
    App::AddParam(rr);

    ParamVariant maxDiffuseBounces;
    maxDiffuseBounces.InitInt(ICON_FA_FILM " Renderer", "Indirect Lighting", "Max Bounces (Non Tr.)",
        fastdelegate::MakeDelegate(this, &IndirectLighting::MaxNonTrBouncesCallback),
        DefaultParamVals::MAX_NON_TR_BOUNCES, 1, 8, 1, "Path Sampling");
    App::AddParam(maxDiffuseBounces);

    ParamVariant maxTransmissionBounces;
    maxTransmissionBounces.InitInt(ICON_FA_FILM " Renderer", "Indirect Lighting", "Max Bounces (Transmissive)",
        fastdelegate::MakeDelegate(this, &IndirectLighting::MaxGlossyTrBouncesCallback),
        DefaultParamVals::MAX_GLOSSY_TR_BOUNCES, 1, 8, 1, "Path Sampling");
    App::AddParam(maxTransmissionBounces);

    //ParamVariant pathRegularization;
    //pathRegularization.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Path Regularization",
    //    fastdelegate::MakeDelegate(this, &IndirectLighting::PathRegularizationCallback),
    //    DefaultParamVals::PATH_REGULARIZATION, "Path Sampling");
    //App::AddParam(pathRegularization);

    ParamVariant texFilter;
    texFilter.InitEnum(ICON_FA_FILM " Renderer", "Indirect Lighting", "Texture Filter",
        fastdelegate::MakeDelegate(this, &IndirectLighting::TexFilterCallback),
        Params::TextureFilter, ZetaArrayLen(Params::TextureFilter), (uint32)DefaultParamVals::TEX_FILTER);
    App::AddParam(texFilter);

    m_method = method;
    m_doTemporalResampling = method == INTEGRATOR::PATH_TRACING ? false : m_doTemporalResampling;

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

    const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
        GlobalResource::RT_SCENE_BVH_CURR);
    const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
        GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);

    m_rootSig.SetRootSRV(2, bvh->GpuVA());
    m_rootSig.SetRootSRV(3, meshInstances->GpuVA());

    m_cbRGI.FinalDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RGI::FINAL_UAV);
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

        // Current temporal reservoir into UAV
        ID3D12Resource* currReservoirs[Reservoir_RGI::NUM] = { m_reservoir_RGI[m_currTemporalIdx].A.Resource(),
            m_reservoir_RGI[m_currTemporalIdx].B.Resource(),
            m_reservoir_RGI[m_currTemporalIdx].C.Resource() };

        for (int i = 0; i < ZetaArrayLen(currReservoirs); i++)
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(currReservoirs[i]));

        // Previous temporal reservoirs into SRV
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
        //SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::SPATIAL_RESAMPLE, m_doSpatialResampling && m_isTemporalReservoirValid);
        Assert(!m_preSampling || m_cbRGI.SampleSetSize_NumSampleSets, "Presampled set params haven't been set.");

        auto srvAIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_0_A_SRV : DESC_TABLE_RGI::RESERVOIR_1_A_SRV;
        auto srvBIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_0_B_SRV : DESC_TABLE_RGI::RESERVOIR_1_B_SRV;
        auto srvCIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_0_C_SRV : DESC_TABLE_RGI::RESERVOIR_1_C_SRV;
        auto uavAIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_1_A_UAV : DESC_TABLE_RGI::RESERVOIR_0_A_UAV;
        auto uavBIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_1_B_UAV : DESC_TABLE_RGI::RESERVOIR_0_B_UAV;
        auto uavCIdx = m_currTemporalIdx == 1 ? DESC_TABLE_RGI::RESERVOIR_1_C_UAV : DESC_TABLE_RGI::RESERVOIR_0_C_UAV;

        m_cbRGI.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvAIdx);
        m_cbRGI.PrevReservoir_B_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvBIdx);
        m_cbRGI.PrevReservoir_C_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvCIdx);
        m_cbRGI.CurrReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavAIdx);
        m_cbRGI.CurrReservoir_B_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavBIdx);
        m_cbRGI.CurrReservoir_C_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavCIdx);

        m_cbRGI.FinalDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RGI::FINAL_UAV);

        const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_SCENE_BVH_CURR);
        const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);

        m_rootSig.SetRootSRV(2, bvh->GpuVA());
        m_rootSig.SetRootSRV(3, meshInstances->GpuVA());

        if (m_useLVG)
        {
            SharedShaderResources& shared = App::GetRenderer().GetSharedShaderResources();
            constexpr auto id = GlobalResource::LIGHT_VOXEL_GRID;
            const auto idHash = XXH3_64bits(id, strlen(id));

            auto* lvg = shared.GetDefaultHeapBuffer(idHash);
            m_rootSig.SetRootSRV(8, lvg->GpuVA());
        }

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
#ifndef NDEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_TtC");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X * 2);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y * 2);

        cb_ReSTIR_PT_Sort cb;
        cb.DispatchDimX = dispatchDimX;
        cb.DispatchDimY = dispatchDimY;
        cb.Reservoir_A_DescHeapIdx = m_cbRPT_Reuse.PrevReservoir_A_DescHeapIdx;
        cb.MapDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_NtC_UAV);
        cb.Flags = m_cbRPT_Reuse.Flags;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_TtC));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
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
#ifndef NDEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_CtT");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X * 2);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y * 2);

        cb_ReSTIR_PT_Sort cb;
        cb.DispatchDimX = dispatchDimX;
        cb.DispatchDimY = dispatchDimY;
        cb.Reservoir_A_DescHeapIdx = m_cbRPT_PathTrace.Reservoir_A_DescHeapIdx;
        cb.MapDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_CtN_UAV);
        cb.Flags = m_cbRPT_Reuse.Flags;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_CtT));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Thread maps into SRV
    {
        D3D12_TEXTURE_BARRIER barriers[(int)SHIFT::COUNT];
        barriers[(int)SHIFT::CtN] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::CtN].Resource());
        barriers[(int)SHIFT::NtC] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::NtC].Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
    }

    // Replay - CtT
    {
#ifndef NDEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_CtT");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

        m_cbRPT_Reuse.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
            (int)DESC_TABLE_RPT::RBUFFER_A_CtN_UAV);
        m_cbRPT_Reuse.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
            (int)DESC_TABLE_RPT::RBUFFER_A_NtC_UAV);

        const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_SCENE_BVH_PREV);
        const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_FRAME_MESH_INSTANCES_PREV);

        m_rootSig.SetRootSRV(2, bvh->GpuVA());
        m_rootSig.SetRootSRV(3, meshInstances->GpuVA());
        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Reuse) / sizeof(DWORD), &m_cbRPT_Reuse);
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_CtT_E : SHADER::ReSTIR_PT_REPLAY_CtT;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Replay - TtC
    {
#ifndef NDEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_TtC");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

        const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_SCENE_BVH_CURR);
        const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);

        m_rootSig.SetRootSRV(2, bvh->GpuVA());
        m_rootSig.SetRootSRV(3, meshInstances->GpuVA());
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_TtC_E : SHADER::ReSTIR_PT_REPLAY_TtC;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Set SRVs for replay buffers
    m_cbRPT_Reuse.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE_RPT::RBUFFER_A_CtN_SRV);
    m_cbRPT_Reuse.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE_RPT::RBUFFER_A_NtC_SRV);

    // r-buffers into SRV
    {
        D3D12_TEXTURE_BARRIER barriers[RBuffer::NUM * (int)SHIFT::COUNT];

        // CtN replay buffers into SRV
        barriers[0] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].A.Resource());
        barriers[1] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].B.Resource());
        barriers[2] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].C.Resource());
        barriers[3] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].D.Resource());

        // NtC replay buffers into SRV
        barriers[4] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].A.Resource());
        barriers[5] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].B.Resource());
        barriers[6] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].C.Resource());
        barriers[7] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].D.Resource());

        computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
    }

    // Reconnect CtT
    {
#ifndef NDEBUG
        computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_CtT");
#endif
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_TEMPORAL_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_TEMPORAL_GROUP_DIM_Y);
        m_cbRPT_Reuse.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;

        const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_SCENE_BVH_PREV);
        const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_FRAME_MESH_INSTANCES_PREV);

        m_rootSig.SetRootSRV(2, bvh->GpuVA());
        m_rootSig.SetRootSRV(3, meshInstances->GpuVA());
        m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Reuse) / sizeof(DWORD), &m_cbRPT_Reuse);
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_CtT_E : SHADER::ReSTIR_PT_RECONNECT_CtT;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    // Reconnect TtC
    {
#ifndef NDEBUG
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

        const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_SCENE_BVH_CURR);
        const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);

        m_rootSig.SetRootSRV(2, bvh->GpuVA());
        m_rootSig.SetRootSRV(3, meshInstances->GpuVA());
        m_rootSig.End(computeCmdList);

        auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_TtC_E : SHADER::ReSTIR_PT_RECONNECT_TtC;
        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
        computeCmdList.PIXEndEvent();
#endif
    }

    gpuTimer.EndQuery(computeCmdList, allQueryIdx);
    computeCmdList.PIXEndEvent();
}

void IndirectLighting::ReSTIR_PT_Spatial(ComputeCmdList& computeCmdList,
    Span<ID3D12Resource*> currTemporalReservoirs,
    Span<ID3D12Resource*> prevTemporalReservoirs)
{
    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();
    const bool emissive = App::GetScene().NumEmissiveInstances() > 0;

    // Current frame's temporal reservoirs become the inputs, previous
    // frame temporal reservoirs become the outputs
    auto inputs = currTemporalReservoirs;
    auto outputs = prevTemporalReservoirs;

    computeCmdList.PIXBeginEvent("ReSTIR_PT_Spatial");
    const uint32_t allQueryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_PT_Spatial");

    for (int pass = 0; pass < m_numSpatialPasses; pass++)
    {
        m_cbRPT_Reuse.Packed = m_cbRPT_Reuse.Packed & ~0xf000;
        m_cbRPT_Reuse.Packed |= ((m_numSpatialPasses << 14) | (pass << 12));

        // Search for reusable spatial neighbor
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_SpatialSearch");
#endif
            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y);

            cb_ReSTIR_PT_SpatialSearch cb;
            cb.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;
            cb.OutputDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_UAV);
            cb.Flags = m_cbRPT_Reuse.Flags;

            m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
            m_rootSig.End(computeCmdList);

            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SPATIAL_SEARCH));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // Barriers
        {
            constexpr int N = Reservoir_RPT::NUM * 2 +      // reservoirs
                1 +                                         // spatial neighbor
                RBuffer::NUM * (int)SHIFT::COUNT +          // r-buffers
                (int)SHIFT::COUNT +                         // thread maps
                1;                                          // target
            SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, N> barriers;

            // Output reservoirs into UAV
            for (int i = 0; i < Reservoir_RPT::NUM; i++)
                barriers.push_back(TextureBarrier_SrvToUavWithSync(outputs[i]));

            // Input reservoirs into SRV
            for (int i = 0; i < Reservoir_RPT::NUM; i++)
                barriers.push_back(TextureBarrier_UavToSrvWithSync(inputs[i]));

            // Spatial neighbor idx into SRV
            barriers.push_back(TextureBarrier_UavToSrvWithSync(m_spatialNeighbor.Resource()));

            // r-buffers into UAV
            for (int i = 0; i < (int)SHIFT::COUNT; i++)
            {
                barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].A.Resource()));
                barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].B.Resource()));
                barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].C.Resource()));
                barriers.push_back(TextureBarrier_SrvToUavWithSync(m_rbuffer[i].D.Resource()));
            }

            // Thread maps into UAV
            if (IS_CB_FLAG_SET(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_SPATIAL))
            {
                barriers.push_back(TextureBarrier_SrvToUavWithSync(m_threadMap[(int)SHIFT::NtC].Resource()));
                barriers.push_back(TextureBarrier_SrvToUavWithSync(m_threadMap[(int)SHIFT::CtN].Resource()));
            }

            // UAV barrier for target
            barriers.push_back(UAVBarrier1(m_rptTarget.Resource()));

            computeCmdList.ResourceBarrier(barriers.data(), (UINT)barriers.size());

            // Update layout
            m_reservoir_RPT[m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
            m_reservoir_RPT[1 - m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;
            m_currTemporalIdx = 1 - m_currTemporalIdx;
        }

        // Sort - CtS
        if (IS_CB_FLAG_SET(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_SPATIAL))
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_CtS");
#endif
            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X * 2);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y * 2);

            cb_ReSTIR_PT_Sort cb;
            cb.DispatchDimX = dispatchDimX;
            cb.DispatchDimY = dispatchDimY;
            cb.Reservoir_A_DescHeapIdx = m_cbRPT_Reuse.Reservoir_A_DescHeapIdx;
            cb.MapDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_CtN_UAV);
            cb.Flags = m_cbRPT_Reuse.Flags;

            m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
            m_rootSig.End(computeCmdList);

            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_CtS));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // Sort - StC
        if (IS_CB_FLAG_SET(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_SPATIAL))
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_Sort_StC");
#endif
            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SORT_GROUP_DIM_X * 2);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SORT_GROUP_DIM_Y * 2);

            cb_ReSTIR_PT_Sort cb;
            cb.DispatchDimX = dispatchDimX;
            cb.DispatchDimY = dispatchDimY;
            cb.Reservoir_A_DescHeapIdx = m_cbRPT_Reuse.Reservoir_A_DescHeapIdx;
            cb.SpatialNeighborHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_SRV);
            cb.MapDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::THREAD_MAP_NtC_UAV);
            cb.Flags = m_cbRPT_Reuse.Flags;

            m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
            m_rootSig.End(computeCmdList);

            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ReSTIR_PT_SORT_StC));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // Replay - CtS
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_CtS");
#endif
            // Thread maps into SRV
            if (IS_CB_FLAG_SET(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_SPATIAL))
            {
                D3D12_TEXTURE_BARRIER barriers[(int)SHIFT::COUNT];
                barriers[(int)SHIFT::CtN] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::CtN].Resource());
                barriers[(int)SHIFT::NtC] = TextureBarrier_UavToSrvWithSync(m_threadMap[(int)SHIFT::NtC].Resource());

                computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
            }

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

            m_cbRPT_Reuse.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
                (int)DESC_TABLE_RPT::RBUFFER_A_CtN_UAV);
            m_cbRPT_Reuse.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
                (int)DESC_TABLE_RPT::RBUFFER_A_NtC_UAV);

            m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Reuse) / sizeof(DWORD), &m_cbRPT_Reuse);
            m_rootSig.End(computeCmdList);

            auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_CtS_E : SHADER::ReSTIR_PT_REPLAY_CtS;
            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // Replay - StC
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_Replay_StC");
#endif
            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_REPLAY_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_REPLAY_GROUP_DIM_Y);

            auto sh = emissive ? SHADER::ReSTIR_PT_REPLAY_StC_E : SHADER::ReSTIR_PT_REPLAY_StC;
            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // r-buffers into SRV
        {
            D3D12_TEXTURE_BARRIER barriers[RBuffer::NUM * (int)SHIFT::COUNT];

            // CtN 
            barriers[0] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].A.Resource());
            barriers[1] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].B.Resource());
            barriers[2] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].C.Resource());
            barriers[3] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::CtN].D.Resource());

            // NtC
            barriers[4] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].A.Resource());
            barriers[5] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].B.Resource());
            barriers[6] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].C.Resource());
            barriers[7] = TextureBarrier_UavToSrvWithSync(m_rbuffer[(int)SHIFT::NtC].D.Resource());

            computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

            // Set SRVs for replay buffers
            m_cbRPT_Reuse.RBufferA_CtN_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
                (int)DESC_TABLE_RPT::RBUFFER_A_CtN_SRV);
            m_cbRPT_Reuse.RBufferA_NtC_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
                (int)DESC_TABLE_RPT::RBUFFER_A_NtC_SRV);
        }

        // Reconnect CtS
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_CtS");
#endif
            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SPATIAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SPATIAL_GROUP_DIM_Y);

            m_cbRPT_Reuse.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;
            m_rootSig.SetRootConstants(0, sizeof(m_cbRPT_Reuse) / sizeof(DWORD), &m_cbRPT_Reuse);
            m_rootSig.End(computeCmdList);

            auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_CtS_E : SHADER::ReSTIR_PT_RECONNECT_CtS;
            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // Reconnect StC
        {
#ifndef NDEBUG
            computeCmdList.PIXBeginEvent("ReSTIR_PT_Reconnect_StC");
#endif
            // UAV barriers for output reservoir B
            D3D12_TEXTURE_BARRIER uavBarrier = UAVBarrier1(outputs[1]);
            computeCmdList.ResourceBarrier(uavBarrier);

            const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_SPATIAL_GROUP_DIM_X);
            const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_SPATIAL_GROUP_DIM_Y);

            auto sh = emissive ? SHADER::ReSTIR_PT_RECONNECT_StC_E : SHADER::ReSTIR_PT_RECONNECT_StC;
            computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)sh));
            computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
#ifndef NDEBUG
            computeCmdList.PIXEndEvent();
#endif
        }

        // Prepare for next iteration (if any)
        if (pass == 0 && m_numSpatialPasses == 2)
        {
            // Spatial neighbor idx into UAV
            auto barrier = TextureBarrier_SrvToUavWithSync(m_spatialNeighbor.Resource());
            computeCmdList.ResourceBarrier(barrier);

            // Swap input and output reservoirs
            std::swap(m_cbRPT_Reuse.PrevReservoir_A_DescHeapIdx,
                m_cbRPT_Reuse.Reservoir_A_DescHeapIdx);
            std::swap(inputs, outputs);
        }
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
    const bool doSpatial = (m_numSpatialPasses > 0) && doTemporal;

    // Initial candidates
    {
        computeCmdList.PIXBeginEvent("ReSTIR_PT_PathTrace");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "ReSTIR_PT_PathTrace");

        constexpr int N = Reservoir_RPT::NUM * 2 +      // reservoir
            RBuffer::NUM * (int)SHIFT::COUNT +          // r-buffers
            (int)SHIFT::COUNT +                         // thread maps
            1;                                          // spatial neighbor
        SmallVector<D3D12_TEXTURE_BARRIER, SystemAllocator, N> textureBarriers;

        // Current reservoirs into UAV
        if (m_reservoir_RPT[m_currTemporalIdx].Layout != D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS)
        {
            for (int i = 0; i < ZetaArrayLen(currReservoirs); i++)
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(currReservoirs[i]));

            m_reservoir_RPT[m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS;
        }

        if (doTemporal)
        {
            // Temporal reservoirs into SRV
            if (m_reservoir_RPT[1 - m_currTemporalIdx].Layout == D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS)
            {
                for (int i = 0; i < ZetaArrayLen(prevReservoirs); i++)
                    textureBarriers.push_back(TextureBarrier_UavToSrvNoSync(prevReservoirs[i]));

                m_reservoir_RPT[1 - m_currTemporalIdx].Layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
            }

            // r-buffers into UAV
            for (int i = 0; i < (int)SHIFT::COUNT; i++)
            {
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].A.Resource()));
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].B.Resource()));
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].C.Resource()));
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_rbuffer[i].D.Resource()));
            }

            // Thread maps into UAV
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_threadMap[(int)SHIFT::CtN].Resource()));
            textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_threadMap[(int)SHIFT::NtC].Resource()));

            if (doSpatial)
                textureBarriers.push_back(TextureBarrier_SrvToUavNoSync(m_spatialNeighbor.Resource()));
        }

        if (!textureBarriers.empty())
            computeCmdList.ResourceBarrier(textureBarriers.data(), (UINT)textureBarriers.size());

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, RESTIR_PT_PATH_TRACE_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, RESTIR_PT_PATH_TRACE_GROUP_DIM_Y);

        SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::TEMPORAL_RESAMPLE, doTemporal);
        SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::SPATIAL_RESAMPLE, doSpatial);
        SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::SPATIAL_RESAMPLE, doSpatial);
        Assert(!m_preSampling || m_cbRPT_PathTrace.SampleSetSize_NumSampleSets,
            "Presampled set params haven't been set.");

        m_cbRPT_PathTrace.DispatchDimX_NumGroupsInTile = ((RESTIR_PT_TILE_WIDTH * dispatchDimY) << 16) | dispatchDimX;
        m_cbRPT_PathTrace.Reservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)uavAIdx);

        const auto& bvh = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_SCENE_BVH_CURR);
        const auto& meshInstances = renderer.GetSharedShaderResources().GetDefaultHeapBuffer(
            GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);

        m_rootSig.SetRootSRV(2, bvh->GpuVA());
        m_rootSig.SetRootSRV(3, meshInstances->GpuVA());
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

    if (doTemporal)
    {
        // Since reservoir descriptors were allocated consecutively, filling just
        // the heap index for A is enough
        m_cbRPT_Reuse.PrevReservoir_A_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)srvAIdx);
        m_cbRPT_Reuse.Reservoir_A_DescHeapIdx = m_cbRPT_PathTrace.Reservoir_A_DescHeapIdx;

        ReSTIR_PT_Temporal(computeCmdList, currReservoirs);
    }

    if (doSpatial)
        ReSTIR_PT_Spatial(computeCmdList, currReservoirs, prevReservoirs);
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
    constexpr int N = 2 * Reservoir_RPT::NUM + 
        (int)SHIFT::COUNT + 
        2 + 
        (int)SHIFT::COUNT * RBuffer::NUM;
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
        list.PushTex2D(ResourceFormats_RPT::RBUFFER_D, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

    list.End();

    m_resHeap = GpuMemory::GetResourceHeap(list.TotalSizeInBytes());
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
            "RPT_Reservoir", i, "E", allocs[currRes++],
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
    static_assert((int)(DESC_TABLE_RPT::RBUFFER_D_CtN_UAV) +
        RBuffer::NUM * 2 == (int)DESC_TABLE_RPT::RBUFFER_D_NtC_UAV);

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
            "RPT_RBuffer", i, i == (int)SHIFT::CtN ? "B_CtN" : "B_NtC",
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
        func(m_rbuffer[i].D, ResourceFormats_RPT::RBUFFER_D,
            "RPT_RBuffer", i, i == (int)SHIFT::CtN ? "D_CtN" : "D_NtC",
            allocs[currRes++],
            (int)DESC_TABLE_RPT::RBUFFER_D_CtN_SRV,
            (int)DESC_TABLE_RPT::RBUFFER_D_CtN_UAV,
            descOffset, layout);
    }

    // Final
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE_RPT::FINAL_UAV));

    // Following never change, so can be set only once
    m_cbRPT_PathTrace.TargetDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::TARGET_UAV);
    m_cbRPT_Reuse.ThreadMap_NtC_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE_RPT::THREAD_MAP_NtC_SRV);
    m_cbRPT_PathTrace.Final = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE_RPT::FINAL_UAV);
    m_cbRPT_Reuse.ThreadMap_CtN_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE_RPT::THREAD_MAP_CtN_SRV);
    m_cbRPT_Reuse.ThreadMap_NtC_DescHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE_RPT::THREAD_MAP_NtC_SRV);
    m_cbRPT_Reuse.SpatialNeighborHeapIdx = m_descTable.GPUDescriptorHeapIndex(
        (int)DESC_TABLE_RPT::SPATIAL_NEIGHBOR_SRV);
    m_cbRPT_Reuse.TargetDescHeapIdx = m_cbRPT_PathTrace.TargetDescHeapIdx;
    m_cbRPT_Reuse.FinalDescHeapIdx = m_cbRPT_PathTrace.Final;

    // Add ReSTIR PT parameters and shader reload handlers
    if (!skipNonResources)
    {
        App::AddShaderReloadHandler("ReSTIR_PT_PathTrace", 
            fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRPT_PathTrace));
        App::AddShaderReloadHandler("ReSTIR_PT_Temporal", 
            fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRPT_Temporal));
        App::AddShaderReloadHandler("ReSTIR_PT_Spatial", 
            fastdelegate::MakeDelegate(this, &IndirectLighting::ReloadRPT_Spatial));

        ParamVariant alphaMin;
        alphaMin.InitFloat(ICON_FA_FILM " Renderer", "Indirect Lighting", "Alpha_min",
            fastdelegate::MakeDelegate(this, &IndirectLighting::AlphaMinCallback),
            DefaultParamVals::ROUGHNESS_MIN, 0.0f, 1.0f, 1e-2f, "Reuse");
        App::AddParam(alphaMin);

        ParamVariant p2;
        p2.InitEnum(ICON_FA_FILM " Renderer", "Indirect Lighting", "Debug View",
            fastdelegate::MakeDelegate(this, &IndirectLighting::DebugViewCallback),
            Params::DebugView, ZetaArrayLen(Params::DebugView), 0, "Reuse");
        App::AddParam(p2);

        ParamVariant doSpatial;
        doSpatial.InitInt(ICON_FA_FILM " Renderer", "Indirect Lighting", "Spatial Resample",
            fastdelegate::MakeDelegate(this, &IndirectLighting::SpatialResamplingCallback),
            m_numSpatialPasses, 0, 2, 1, "Reuse");
        App::AddParam(doSpatial);

        ParamVariant sortTemporal;
        sortTemporal.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Sort (Temporal)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::SortTemporalCallback), true, "Reuse");
        App::AddParam(sortTemporal);

        ParamVariant sortSpatial;
        sortSpatial.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Sort (Spatial)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::SortSpatialCallback), true, "Reuse");
        App::AddParam(sortSpatial);

        ParamVariant doTemporal;
        doTemporal.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Temporal Resample",
            fastdelegate::MakeDelegate(this, &IndirectLighting::TemporalResamplingCallback),
            m_doTemporalResampling, "Reuse");
        App::AddParam(doTemporal);

        ParamVariant maxTemporalM;
        maxTemporalM.InitInt(ICON_FA_FILM " Renderer", "Indirect Lighting", "M_max (Temporal)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::M_maxTCallback),
            DefaultParamVals::M_MAX, 1, 15, 1, "Reuse");
        App::AddParam(maxTemporalM);

        ParamVariant maxSpatialM;
        maxSpatialM.InitInt(ICON_FA_FILM " Renderer", "Indirect Lighting", "M_max (Spatial)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::M_maxSCallback),
            DefaultParamVals::M_MAX_SPATIAL, 1, 12, 1, "Reuse");
        App::AddParam(maxSpatialM);

        ParamVariant suppressOutliers;
        suppressOutliers.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Boiling Suppression",
            fastdelegate::MakeDelegate(this, &IndirectLighting::BoilingSuppressionCallback),
            DefaultParamVals::BOILING_SUPPRESSION, "Reuse");
        App::AddParam(suppressOutliers);
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
        m_rbuffer[i].D.Reset();

        m_threadMap[i].Reset();
    }

    m_spatialNeighbor.Reset();
    m_rptTarget.Reset();
    m_resHeap.Reset();

    // Remove parameters and shader reload handlers
    App::RemoveShaderReloadHandler("ReSTIR_PT_PathTrace");
    App::RemoveShaderReloadHandler("ReSTIR_PT_Temporal");
    App::RemoveShaderReloadHandler("ReSTIR_PT_Spatial");
    App::RemoveShaderReloadHandler("ReSTIR_PT_SpatialSearch");

    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Alpha_min");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Debug View");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "M_max (Temporal)");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "M_max (Spatial)");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Spatial Resample");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Sort (Temporal)");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Sort (Spatial)");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Temporal Resample");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Boiling Suppression");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Lower M-cap Disoccluded");
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

    PlacedResourceList<2 * Reservoir_RGI::NUM > list;

    for (int i = 0; i < 2; i++)
    {
        list.PushTex2D(ResourceFormats_RGI::RESERVOIR_A, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RGI::RESERVOIR_B, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
        list.PushTex2D(ResourceFormats_RGI::RESERVOIR_C, w, h, TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);
    }

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

    // Final
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE_RGI::FINAL_UAV));

    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, DefaultParamVals::STOCHASTIC_MULTI_BOUNCE);

    // Add ReSTIR GI parameters and shader reload handlers
    if (!skipNonResources)
    {
        ParamVariant stochasticMultibounce;
        stochasticMultibounce.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Stochastic Multi-bounce",
            fastdelegate::MakeDelegate(this, &IndirectLighting::StochasticMultibounceCallback),
            DefaultParamVals::STOCHASTIC_MULTI_BOUNCE, "Path Sampling");
        App::AddParam(stochasticMultibounce);

        ParamVariant doTemporal;
        doTemporal.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Temporal Resample",
            fastdelegate::MakeDelegate(this, &IndirectLighting::TemporalResamplingCallback),
            m_doTemporalResampling, "Reuse");
        App::AddParam(doTemporal);

        ParamVariant maxM;
        maxM.InitInt(ICON_FA_FILM " Renderer", "Indirect Lighting", "M_max (Temporal)",
            fastdelegate::MakeDelegate(this, &IndirectLighting::M_maxTCallback),
            DefaultParamVals::M_MAX, 1, 15, 1, "Reuse");
        App::AddParam(maxM);

        ParamVariant suppressOutliers;
        suppressOutliers.InitBool(ICON_FA_FILM " Renderer", "Indirect Lighting", "Boiling Suppression",
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
    }

    m_resHeap.Reset();

    App::RemoveShaderReloadHandler("ReSTIR_GI");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Stochastic Multi-bounce");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "M_max (Temporal)");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Boiling Suppression");
    App::RemoveParam(ICON_FA_FILM " Renderer", "Indirect Lighting", "Temporal Resample");
}

void IndirectLighting::SwitchToPathTracer(bool skipNonResources)
{
    Direct3DUtil::CreateTexture2DUAV(m_final, m_descTable.CPUHandle((int)DESC_TABLE_RGI::FINAL_UAV));
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
            ResourceFormats_RGI::FINAL,
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

void IndirectLighting::MaxNonTrBouncesCallback(const Support::ParamVariant& p)
{
    const auto newVal = (uint16_t)p.GetInt().m_value;
    m_cbRGI.MaxNonTrBounces = newVal;
    constexpr uint32 ONES_COMP = ~(0xfu << PACKED_INDEX::NUM_DIFFUSE_BOUNCES);
    m_cbRPT_PathTrace.Packed = m_cbRPT_PathTrace.Packed & ONES_COMP;
    m_cbRPT_PathTrace.Packed |= newVal;
    m_cbRPT_Reuse.Packed = m_cbRPT_PathTrace.Packed;

    App::GetScene().SceneModified();
}

void IndirectLighting::MaxGlossyTrBouncesCallback(const Support::ParamVariant& p)
{
    const auto newVal = (uint16_t)p.GetInt().m_value;
    m_cbRGI.MaxGlossyTrBounces = newVal;
    constexpr uint32 ONES_COMP = ~(0xfu << PACKED_INDEX::NUM_GLOSSY_BOUNCES);
    m_cbRPT_PathTrace.Packed = m_cbRPT_PathTrace.Packed & ONES_COMP;
    m_cbRPT_PathTrace.Packed |= (newVal << PACKED_INDEX::NUM_GLOSSY_BOUNCES);
    m_cbRPT_Reuse.Packed = m_cbRPT_PathTrace.Packed;

    App::GetScene().SceneModified();
}

void IndirectLighting::StochasticMultibounceCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::STOCHASTIC_MULTI_BOUNCE, p.GetBool());

    App::GetScene().SceneModified();
}

void IndirectLighting::RussianRouletteCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::RUSSIAN_ROULETTE, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::RUSSIAN_ROULETTE, p.GetBool());

    App::GetScene().SceneModified();
}

void IndirectLighting::TemporalResamplingCallback(const Support::ParamVariant& p)
{
    m_doTemporalResampling = p.GetBool();
    App::GetScene().SceneModified();
}

void IndirectLighting::SpatialResamplingCallback(const Support::ParamVariant& p)
{
    m_numSpatialPasses = p.GetInt().m_value;
    App::GetScene().SceneModified();
}

void IndirectLighting::M_maxTCallback(const Support::ParamVariant& p)
{
    auto newM = (uint16_t)p.GetInt().m_value;
    m_cbRGI.M_max = newM;
    constexpr uint32 ONES_COMP = ~(0xfu << PACKED_INDEX::MAX_TEMPORAL_M);
    m_cbRPT_PathTrace.Packed = (m_cbRPT_PathTrace.Packed & ONES_COMP);
    m_cbRPT_PathTrace.Packed |= (newM << PACKED_INDEX::MAX_TEMPORAL_M);
    m_cbRPT_Reuse.Packed = m_cbRPT_PathTrace.Packed;

    App::GetScene().SceneModified();
}

void IndirectLighting::M_maxSCallback(const Support::ParamVariant& p)
{
    auto newM = (uint16_t)p.GetInt().m_value;
    constexpr uint32 ONES_COMP = ~(0xfu << PACKED_INDEX::MAX_SPATIAL_M);
    m_cbRPT_PathTrace.Packed = (m_cbRPT_PathTrace.Packed & ONES_COMP);
    m_cbRPT_PathTrace.Packed |= (newM << PACKED_INDEX::MAX_SPATIAL_M);
    m_cbRPT_Reuse.Packed = m_cbRPT_PathTrace.Packed;

    App::GetScene().SceneModified();
}

void IndirectLighting::DebugViewCallback(const Support::ParamVariant& p)
{
    auto newVal = (uint16_t)p.GetEnum().m_curr;
    constexpr uint32 ONES_COMP = ~(0xf << PACKED_INDEX::DEBUG_VIEW);
    m_cbRPT_PathTrace.Packed = (m_cbRPT_PathTrace.Packed & ONES_COMP);
    m_cbRPT_PathTrace.Packed |= (newVal << PACKED_INDEX::DEBUG_VIEW);
    m_cbRPT_Reuse.Packed = m_cbRPT_PathTrace.Packed;
}

void IndirectLighting::SortTemporalCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::SORT_TEMPORAL, p.GetBool());
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_TEMPORAL, p.GetBool());
}

void IndirectLighting::SortSpatialCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::SORT_SPATIAL, p.GetBool());
}

void IndirectLighting::TexFilterCallback(const Support::ParamVariant& p)
{
    auto newVal = EnumToSamplerIdx((TEXTURE_FILTER)p.GetEnum().m_curr);
    constexpr uint32 ONES_COMP = ~(0xfu << PACKED_INDEX::TEX_FILTER);
    m_cbRPT_PathTrace.TexFilterDescHeapIdx = newVal;
    m_cbRPT_PathTrace.Packed = (m_cbRPT_PathTrace.Packed & ONES_COMP);
    m_cbRPT_PathTrace.Packed |= (newVal << PACKED_INDEX::TEX_FILTER);
    m_cbRPT_Reuse.Packed = m_cbRPT_PathTrace.Packed;

    App::GetScene().SceneModified();
}

void IndirectLighting::BoilingSuppressionCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::BOILING_SUPPRESSION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::BOILING_SUPPRESSION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::BOILING_SUPPRESSION, p.GetBool());

    App::GetScene().SceneModified();
}

void IndirectLighting::PathRegularizationCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbRGI, CB_IND_FLAGS::PATH_REGULARIZATION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_PathTrace, CB_IND_FLAGS::PATH_REGULARIZATION, p.GetBool());
    SET_CB_FLAG(m_cbRPT_Reuse, CB_IND_FLAGS::PATH_REGULARIZATION, p.GetBool());

    App::GetScene().SceneModified();
}

void IndirectLighting::AlphaMinCallback(const Support::ParamVariant& p)
{
    float newVal = p.GetFloat().m_value;
    m_cbRPT_PathTrace.Alpha_min = m_cbRPT_Reuse.Alpha_min = 
        newVal * newVal;

    App::GetScene().SceneModified();
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
    TaskSet ts;

    ts.EmplaceTask("Reload_Reconnect_CtT", [this]()
        {
            auto sh = SHADER::ReSTIR_PT_RECONNECT_CtT;
            const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_CtT.hlsl";

            if (App::GetScene().NumEmissiveInstances() > 0)
            {
                sh = SHADER::ReSTIR_PT_RECONNECT_CtT_E;
                p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_CtT_E.hlsl";
            }

            const int i = (int)sh;
            m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);
        });

    ts.EmplaceTask("Reload_Reconnect_TtC", [this]()
        {
            auto sh = SHADER::ReSTIR_PT_RECONNECT_TtC;
            const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_TtC.hlsl";

            if (App::GetScene().NumEmissiveInstances() > 0)
            {
                sh = SHADER::ReSTIR_PT_RECONNECT_TtC_E;
                p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_TtC_E.hlsl";
            }

            const int i = (int)sh;
            m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);
        });

    ts.EmplaceTask("Reload_Replay_CtT", [this]()
        {
            auto sh = SHADER::ReSTIR_PT_REPLAY_CtT;
            const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Replay.hlsl";

            if (App::GetScene().NumEmissiveInstances() > 0)
            {
                sh = SHADER::ReSTIR_PT_REPLAY_CtT_E;
                p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_E.hlsl";
            }

            const int i = (int)sh;
            m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);
        });

    WaitObject waitObj;
    ts.Sort();
    ts.Finalize(&waitObj);
    App::Submit(ZetaMove(ts));

    auto sh = SHADER::ReSTIR_PT_REPLAY_TtC;
    const char* p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_TtC.hlsl";

    if (App::GetScene().NumEmissiveInstances() > 0)
    {
        sh = SHADER::ReSTIR_PT_REPLAY_TtC_E;
        p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_TtC_E.hlsl";
    }

    const int i = (int)sh;
    m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);

    waitObj.Wait();
}

void IndirectLighting::ReloadRPT_Spatial()
{
    TaskSet ts;

    ts.EmplaceTask("Reload_Reconnect_CtS", [this]()
        {
            auto sh = SHADER::ReSTIR_PT_RECONNECT_CtS;
            const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_CtS.hlsl";

            if (App::GetScene().NumEmissiveInstances() > 0)
            {
                sh = SHADER::ReSTIR_PT_RECONNECT_CtS_E;
                p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_CtS_E.hlsl";
            }

            const int i = (int)sh;
            m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);
        });

    ts.EmplaceTask("Reload_Reconnect_StC", [this]()
        {
            auto sh = SHADER::ReSTIR_PT_RECONNECT_StC;
            const char* p = "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_Reconnect_StC.hlsl";

            if (App::GetScene().NumEmissiveInstances() > 0)
            {
                sh = SHADER::ReSTIR_PT_RECONNECT_StC_E;
                p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Reconnect_StC_E.hlsl";
            }

            const int i = (int)sh;
            m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);
        });

    ts.EmplaceTask("Reload_Replay_CtS", [this]()
        {
            auto sh = SHADER::ReSTIR_PT_REPLAY_CtS;
            const char* p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_CtS.hlsl";

            if (App::GetScene().NumEmissiveInstances() > 0)
            {
                sh = SHADER::ReSTIR_PT_REPLAY_CtS_E;
                p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_CtS_E.hlsl";
            }

            const int i = (int)sh;
            m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);
        });

    WaitObject waitObj;
    ts.Sort();
    ts.Finalize(&waitObj);
    App::Submit(ZetaMove(ts));

    auto sh = SHADER::ReSTIR_PT_REPLAY_StC;
    const char* p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_StC.hlsl";

    if (App::GetScene().NumEmissiveInstances() > 0)
    {
        sh = SHADER::ReSTIR_PT_REPLAY_StC_E;
        p = "IndirectLighting\\ReSTIR_PT\\Variants\\ReSTIR_PT_Replay_StC_E.hlsl";
    }

    const int i = (int)sh;
    m_psoLib.Reload(i, m_rootSigObj.Get(), p, false);

    waitObj.Wait();
}

void IndirectLighting::ReloadRPT_SpatialSearch()
{
    const int i = (int)SHADER::ReSTIR_PT_SPATIAL_SEARCH;
    m_psoLib.Reload(i, m_rootSigObj.Get(), 
        "IndirectLighting\\ReSTIR_PT\\ReSTIR_PT_SpatialSearch.hlsl");
}

