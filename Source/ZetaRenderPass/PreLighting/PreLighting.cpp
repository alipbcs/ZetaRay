#include "PreLighting.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>
#include <Math/Sampling.h>
#include <Scene/SceneCore.h>
#include <Core/RenderGraph.h>
#include <Core/SharedShaderResources.h>
#include <Math/Sampling.h>
#include <Support/Task.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;
using namespace ZetaRay::Math;

namespace
{
    // Ref: https://www.keithschwarz.com/darts-dice-coins/
    void BuildAliasTable(MutableSpan<float> probs, MutableSpan<RT::EmissiveLumenAliasTableEntry> table)
    {
        const int64_t N = probs.size();
        const float oneDivN = 1.0f / N;
        AliasTable_Normalize(probs);

        for (int64_t i = 0; i < N; i++)
        {
            table[i].CachedP_Orig = probs[i] * oneDivN;
#ifdef _DEBUG
            table[i].Alias = uint32_t(-1);
#endif
        }

        // maintain an index buffer since original ordering of elements must be preserved
        SmallVector<uint32_t, App::LargeFrameAllocator> larger;
        larger.reserve(N);

        SmallVector<uint32_t, App::LargeFrameAllocator> smaller;
        smaller.reserve(N);

        for (int64_t i = 0; i < N; i++)
        {
            if (probs[i] < 1.0f)
                smaller.push_back((uint32_t)i);
            else
                larger.push_back((uint32_t)i);
        }

#ifdef _DEBUG
        int64_t numInsertions = 0;
#endif // _DEBUG

        // in each iteration, pick two probabilities such that one is smaller than 1.0 and the other larger 
        // than 1.0. Use the latter to bring up the former to 1.0.
        while (!smaller.empty() && !larger.empty())
        {
            const uint32_t smallerIdx = smaller.back();
            smaller.pop_back();
            const float smallerProb = probs[smallerIdx];

            const uint32_t largerIdx = larger.back();
            float largerProb = probs[largerIdx];
            Assert(largerProb >= 1.0f, "should be >= 1.0");

            RT::EmissiveLumenAliasTableEntry& e = table[smallerIdx];
            Assert(e.Alias == uint32_t(-1), "Every element must be inserted exactly one time.");
            e.Alias = largerIdx;
            e.P_Curr = smallerProb;

            // = largerProb - (1.0f - smallerProb);
            largerProb = (smallerProb + largerProb) - 1.0f;
            probs[largerIdx] = largerProb;

            if (largerProb < 1.0f)
            {
                larger.pop_back();
                smaller.push_back(largerIdx);
            }

#ifdef _DEBUG
            numInsertions++;
#endif
        }

        while (!larger.empty())
        {
            size_t idx = larger.back();
            larger.pop_back();
            Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

            // alias should point to itself
            table[idx].Alias = (uint32_t)idx;
            table[idx].P_Curr = 1.0f;

#ifdef _DEBUG
            numInsertions++;
#endif
        }

        while (!smaller.empty())
        {
            size_t idx = smaller.back();
            smaller.pop_back();
            Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

            // alias should point to itself
            table[idx].Alias = (uint32_t)idx;
            table[idx].P_Curr = 1.0f;

#ifdef _DEBUG
            numInsertions++;
#endif
        }

        Assert(numInsertions == N, "Some elements were not inserted.");

        for (int64_t i = 0; i < N; i++)
            table[i].CachedP_Alias = table[table[i].Alias].CachedP_Orig;
    }
}

//--------------------------------------------------------------------------------------
// PreLighting
//--------------------------------------------------------------------------------------

PreLighting::PreLighting()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // root constants
    m_rootSig.InitAsConstants(0,
        NUM_CONSTS,
        0);

    // frame constants
    m_rootSig.InitAsCBV(1,
        1,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // emissive triangles
    m_rootSig.InitAsBufferSRV(2,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::EMISSIVE_TRIANGLE_BUFFER,
        true);

    // alias table
    m_rootSig.InitAsBufferSRV(3,
        1,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE,
        true);

    // halton
    m_rootSig.InitAsBufferSRV(4,
        2,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        nullptr,
        true);

    // lumen/sample sets
    m_rootSig.InitAsBufferUAV(5,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
        nullptr,
        true);
}

PreLighting::~PreLighting()
{
    Reset();
}

void PreLighting::Init()
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
    RenderPassBase::InitRenderPass("PreLighting", flags, samplers);

    TaskSet ts;

    for (int i = 0; i < (int)SHADERS::COUNT; i++)
    {
        StackStr(buff, n, "PreLighting_shader_%d", i);

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

    float2 samples[ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI];

    for (int i = 0; i < ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI; i++)
    {
        samples[i].x = Halton(i + 1, 2);
        samples[i].y = Halton(i + 1, 3);
    }

    m_halton = GpuMemory::GetDefaultHeapBufferAndInit("Halton",
        ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI * sizeof(float2),
        false,
        samples);

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
    CreateOutputs();

    ParamVariant extents;
    extents.InitFloat3("Renderer", "Lighting", "Extents",
        fastdelegate::MakeDelegate(this, &PreLighting::VoxelExtentsCallback),
        m_voxelExtents,
        0.1,
        2,
        0.1);
    App::AddParam(extents);

    ParamVariant offset_y;
    offset_y.InitFloat("Renderer", "Lighting", "Y Offset",
        fastdelegate::MakeDelegate(this, &PreLighting::YOffsetCallback),
        m_yOffset,
        0,
        2,
        0.1);
    App::AddParam(offset_y);

    App::AddShaderReloadHandler("BuildLightVoxelGrid", fastdelegate::MakeDelegate(this, &PreLighting::ReloadBuildLVG));
}

void PreLighting::Reset()
{
    if (IsInitialized())
    {
        for (int i = 0; i < ZetaArrayLen(m_psos); i++)
            m_psos[i] = nullptr;

        m_halton.Reset();
        m_lumen.Reset();
        m_readback.Reset();
        m_curvature.Reset();

        RenderPassBase::ResetRenderPass();
    }
}

void PreLighting::OnWindowResized()
{
    CreateOutputs();
}

void PreLighting::Update(bool emissiveLighting)
{
    m_estimateLumenThisFrame = false;
    m_doPresamplingThisFrame = false;

    if (!emissiveLighting)
        return;

    if (m_currNumTris && !m_lvg.IsInitialized())
    {
        const size_t sizeInBytes = NUM_SAMPLES_PER_VOXEL * m_voxelGridDim.x * m_voxelGridDim.y * m_voxelGridDim.z *
            sizeof(RT::VoxelSample);

        m_lvg = GpuMemory::GetDefaultHeapBuffer("LVG",
            (uint32_t)sizeInBytes,
            D3D12_RESOURCE_STATE_COMMON,
            true);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::LIGHT_VOXEL_GRID, m_lvg);
    }

    if (App::GetScene().AreEmissivesStale())
    {
        m_estimateLumenThisFrame = true;

        const size_t currLumenBuffLen = m_lumen.IsInitialized() ? m_lumen.Desc().Width / sizeof(float) : 0;
        m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();
        Assert(m_currNumTris, "Redundant call.");

        if (currLumenBuffLen < m_currNumTris)
        {
            const uint32_t sizeInBytes = m_currNumTris * sizeof(float);

            // allocate a GPU buffer conatining lumen estimates per triangle
            m_lumen = GpuMemory::GetDefaultHeapBuffer("TriLumen",
                sizeInBytes,
                D3D12_RESOURCE_STATE_COMMON,
                true);

            // allocate a readback buffer so the results can be read back on CPU
            m_readback = GpuMemory::GetReadbackHeapBuffer(sizeInBytes);
        }

        return;
    }

    // light presampling is not effective when number of lights is low
    if (m_currNumTris < DefaultParamVals::MIN_NUM_LIGHTS_PRESAMPLING)
        return;

    // since the code reached there, we know emissive lighting is enabled and number of lights
    // exceeds the threashold
    m_doPresamplingThisFrame = true;

    if (!m_sampleSets.IsInitialized())
    {
        constexpr size_t sizeInBytes = DefaultParamVals::NUM_SAMPLE_SETS * DefaultParamVals::SAMPLE_SET_SIZE * 
            sizeof(RT::PresampledEmissiveTriangle);

        m_sampleSets = GpuMemory::GetDefaultHeapBuffer("EmissiveSampleSets",
            sizeInBytes,
            D3D12_RESOURCE_STATE_COMMON,
            true);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::PRESAMPLED_EMISSIVE_SETS, m_sampleSets);
    }
}

void PreLighting::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    if (m_estimateLumenThisFrame)
    {
        Assert(m_readback.IsInitialized(), "no readback buffer.");
        Assert(!m_readback.IsMapped(), "readback buffer can't be mapped while in use by the GPU.");
        Assert(m_lumen.IsInitialized(), "no lumen buffer.");

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(m_currNumTris, ESTIMATE_TRI_LUMEN_NUM_TRIS_PER_GROUP);
        Assert(dispatchDimX <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, "#blocks exceeded maximum allowed.");

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "EstimateTriLumen");

        computeCmdList.PIXBeginEvent("EstimateTriLumen");
        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::ESTIMATE_TRIANGLE_LUMEN]);

        computeCmdList.ResourceBarrier(m_lumen.Resource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_rootSig.SetRootSRV(4, m_halton.GpuVA());
        m_rootSig.SetRootUAV(5, m_lumen.GpuVA());

        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(dispatchDimX, 1, 1);

        // copy results to readback buffer, so alias table can be computed on the cpu
        computeCmdList.ResourceBarrier(m_lumen.Resource(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

        computeCmdList.CopyBufferRegion(m_readback.Resource(),
            0,
            m_lumen.Resource(),
            0,
            m_currNumTris * sizeof(float));

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        computeCmdList.PIXEndEvent();
    }

    if (m_doPresamplingThisFrame)
    {
        constexpr uint32_t numSamples = DefaultParamVals::NUM_SAMPLE_SETS * DefaultParamVals::SAMPLE_SET_SIZE;
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(numSamples, PRESAMPLE_EMISSIVE_GROUP_DIM_X);
        Assert(dispatchDimX <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, "#blocks exceeded maximum allowed.");

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "PresampleEmissives");

        computeCmdList.PIXBeginEvent("PresampleEmissives");
        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::PRESAMPLING]);

        // "buffers MAY be initially accessed in an ExecuteCommandLists scope without a Barrier...Additionally, a buffer 
        // or texture using a queue-specific common layout can use D3D12_BARRIER_ACCESS_UNORDERED_ACCESS without a barrier."
        // Ref: https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html
#if 0
        D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_sampleSets.Resource(),
            D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

        computeCmdList.ResourceBarrier(barrier);
#endif

        cbPresampling cb;
        cb.NumTotalSamples = DefaultParamVals::NUM_SAMPLE_SETS * DefaultParamVals::SAMPLE_SET_SIZE;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.SetRootUAV(5, m_sampleSets.GpuVA());

        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(dispatchDimX, 1, 1);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        cmdList.PIXEndEvent();
    }

    // estimate curvature
    {
        const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, ESTIMATE_CURVATURE_GROUP_DIM_X);
        const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, ESTIMATE_CURVATURE_GROUP_DIM_Y);

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "EstimateCurvature");

        computeCmdList.PIXBeginEvent("EstimateCurvature");
        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::ESTIMATE_CURVATURE]);

        cbCurvature cb;
        cb.OutputUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::CURVATURE_UAV);

        m_rootSig.SetRootConstants(0, sizeof(cbCurvature) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        cmdList.PIXEndEvent();
    }

    if (m_buildLVGThisFrame)
    {
        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "LightVoxelGrid");

        computeCmdList.PIXBeginEvent("LightVoxelGrid");
        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::BUILD_LIGHT_VOXEL_GRID]);

        cbLVG cb;
        cb.GridDim_x = m_voxelGridDim.x;
        cb.GridDim_y = m_voxelGridDim.y;
        cb.GridDim_z = m_voxelGridDim.z;
        cb.Extents_x = m_voxelExtents.x;
        cb.Extents_y = m_voxelExtents.y;
        cb.Extents_z = m_voxelExtents.z;
        cb.NumTotalSamples = NUM_SAMPLES_PER_VOXEL * m_voxelGridDim.x * m_voxelGridDim.y * m_voxelGridDim.z;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.SetRootUAV(5, m_lvg.GpuVA());
        m_rootSig.End(computeCmdList);

        computeCmdList.Dispatch(m_voxelGridDim.x, m_voxelGridDim.y, m_voxelGridDim.z);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        cmdList.PIXEndEvent();
    }
}

void PreLighting::CreateOutputs()
{
    auto& renderer = App::GetRenderer();

    m_curvature = GpuMemory::GetTexture2D("Curvature",
        renderer.GetRenderWidth(), renderer.GetRenderHeight(),
        ResourceFormats::CURVATURE,
        D3D12_RESOURCE_STATE_COMMON,
        CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    Direct3DUtil::CreateTexture2DUAV(m_curvature, m_descTable.CPUHandle((int)DESC_TABLE::CURVATURE_UAV));
}

void PreLighting::ReleaseLumenBufferAndReadback()
{
    m_lumen.Reset();
    m_readback.Reset();
    m_buildLVGThisFrame = true;
}

void PreLighting::ReloadBuildLVG()
{
    const int i = (int)SHADERS::BUILD_LIGHT_VOXEL_GRID;

    m_psoLib.Reload(i, "PreLighting\\BuildLightVoxelGrid.hlsl", true);
    m_psos[i] = m_psoLib.GetComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void PreLighting::VoxelExtentsCallback(const Support::ParamVariant& p)
{
    m_voxelExtents = p.GetFloat3().m_val;
}

void PreLighting::YOffsetCallback(const Support::ParamVariant& p)
{
    m_yOffset = p.GetFloat().m_val;
}

//--------------------------------------------------------------------------------------
// EmissiveTriangleAliasTable
//--------------------------------------------------------------------------------------

void EmissiveTriangleAliasTable::Update(ReadbackHeapBuffer* readback)
{
    Assert(readback, "null resource.");
    m_readback = readback;

    const size_t currBuffLen = m_aliasTable.IsInitialized() ? m_aliasTable.Desc().Width / sizeof(float) : 0;
    m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();
    Assert(m_currNumTris, "redundant call.");

    if (currBuffLen < m_currNumTris)
    {
        m_aliasTable = GpuMemory::GetDefaultHeapBuffer("AliasTable",
            m_currNumTris * sizeof(RT::EmissiveLumenAliasTableEntry),
            D3D12_RESOURCE_STATE_COMMON,
            false);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE, m_aliasTable);
    }
}

void EmissiveTriangleAliasTable::SetEmissiveTriPassHandle(Core::RenderNodeHandle& emissiveTriHandle)
{
    Assert(emissiveTriHandle.IsValid(), "invalid handle.");
    m_emissiveTriHandle = emissiveTriHandle.Val;
}

void EmissiveTriangleAliasTable::Render(CommandList& cmdList)
{
    Assert(m_readback, "Readback buffer hasn't been set.");
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& scene = App::GetScene();

    SmallVector<RT::EmissiveLumenAliasTableEntry, App::LargeFrameAllocator> table;
    table.resize(m_currNumTris);

    const uint64_t fence = scene.GetRenderGraph()->GetCompletionFence(RenderNodeHandle(m_emissiveTriHandle));
    Assert(fence != uint64_t(-1), "invalid fence value.");

    // wait until GPU finishes copying data to readback buffer
    //renderer.WaitForComputeQueueFenceCPU(fence);
    renderer.WaitForDirectQueueFenceCPU(fence);

    {
        // safe to map, related fence has passed
        m_readback->Map();

        float* data = reinterpret_cast<float*>(m_readback->MappedMemory());
        BuildAliasTable(MutableSpan(data, m_currNumTris), table);

        // unmapping happens automatically when readback buffer is released
        //m_readback->Unmap();
    }

    auto& gpuTimer = renderer.GetGpuTimer();

    // record the timestamp prior to execution
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "UploadAliasTable");

    computeCmdList.PIXBeginEvent("UploadAliasTable");

    // schedule a copy
    const uint32_t sizeInBytes = sizeof(RT::EmissiveLumenAliasTableEntry) * m_currNumTris;
    m_aliasTableUpload = GpuMemory::GetUploadHeapBuffer(sizeInBytes);
    m_aliasTableUpload.Copy(0, sizeInBytes, table.data());
    computeCmdList.CopyBufferRegion(m_aliasTable.Resource(), 
        0,
        m_aliasTableUpload.Resource(),
        m_aliasTableUpload.Offset(),
        sizeInBytes);

    computeCmdList.ResourceBarrier(m_aliasTable.Resource(), 
        D3D12_RESOURCE_STATE_COPY_DEST, 
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    // record the timestamp after execution
    gpuTimer.EndQuery(computeCmdList, queryIdx);

    cmdList.PIXEndEvent();

    // even though at this point this command list hasn't been submitted yet (only recorded),
    // it's safe to release the buffers here -- this is because resource deallocation
    // and signalling the related fence happens at the end of frame when all command 
    // lists have been submitted
    m_releaseDlg();
}
