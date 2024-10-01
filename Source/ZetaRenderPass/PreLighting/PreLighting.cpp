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
#include <App/Timer.h>
#include <App/Log.h>

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
#ifndef NDEBUG
            table[i].Alias = UINT32_MAX;
#endif
        }

        // maintain an index buffer since original ordering of elements must be preserved
        SmallVector<uint32_t, App::OneTimeFrameAllocatorWithFallback> larger;
        larger.reserve(N);

        SmallVector<uint32_t, App::OneTimeFrameAllocatorWithFallback> smaller;
        smaller.reserve(N);

        for (int64_t i = 0; i < N; i++)
        {
            if (probs[i] < 1.0f)
                smaller.push_back((uint32_t)i);
            else
                larger.push_back((uint32_t)i);
        }

#ifndef NDEBUG
        int64_t numInsertions = 0;
#endif

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

#ifndef NDEBUG
            numInsertions++;
#endif
        }

        while (!larger.empty())
        {
            size_t idx = larger.back();
            larger.pop_back();
            //Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

            // alias should point to itself
            table[idx].Alias = (uint32_t)idx;
            table[idx].P_Curr = 1.0f;

#ifndef NDEBUG
            numInsertions++;
#endif
        }

        while (!smaller.empty())
        {
            size_t idx = smaller.back();
            smaller.pop_back();
            //Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

            // alias should point to itself
            table[idx].Alias = (uint32_t)idx;
            table[idx].P_Curr = 1.0f;

#ifndef NDEBUG
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
    m_rootSig.InitAsConstants(0, NUM_CONSTS, 0);

    // frame constants
    m_rootSig.InitAsCBV(1, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // emissive triangles
    m_rootSig.InitAsBufferSRV(2, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::EMISSIVE_TRIANGLE_BUFFER,
        true);

    // alias table
    m_rootSig.InitAsBufferSRV(3, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE,
        true);

    // halton
    m_rootSig.InitAsBufferSRV(4, 2, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        nullptr,
        true);

    // lumen/sample sets
    m_rootSig.InitAsBufferUAV(5, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
        nullptr,
        true);
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

    for (int i = 0; i < (int)SHADER::COUNT; i++)
    {
        StackStr(buff, n, "PreLighting_shader_%d", i);

        ts.EmplaceTask(buff, [i, this]()
            {
                m_psoLib.CompileComputePSO_MT(i, m_rootSigObj.Get(), COMPILED_CS[i]);
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
}

void PreLighting::Update()
{
    m_estimateLumenThisFrame = false;
    m_doPresamplingThisFrame = false;
    m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();
    m_useLVG = m_useLVG && (m_currNumTris >= m_minNumLightsForPresampling);

    if (m_currNumTris == 0)
        return;

    const bool isLVGAllocated = m_lvg.IsInitialized();
    if ((m_useLVG && !isLVGAllocated) || (!m_useLVG && isLVGAllocated))
        ToggleLVG();

    if (App::GetScene().AreEmissivesStale())
    {
        m_estimateLumenThisFrame = true;
        const size_t currLumenBuffLen = m_lumen.IsInitialized() ? 
            m_lumen.Desc().Width / sizeof(float) : 0;

        if (currLumenBuffLen < m_currNumTris)
        {
            const uint32_t sizeInBytes = m_currNumTris * sizeof(float);

            // GPU buffer containing lumen estimates per triangle
            m_lumen = GpuMemory::GetDefaultHeapBuffer("TriLumen",
                sizeInBytes,
                D3D12_RESOURCE_STATE_COMMON,
                true);

            // Readback buffer to read results on CPU
            m_readback = GpuMemory::GetReadbackHeapBuffer(sizeInBytes);
        }

        return;
    }

    // Skip light presampling when number of emissives is low
    Assert(m_minNumLightsForPresampling != UINT32_MAX, 
        "Light presampling is enabled, but presampling params haven't been set.");
    if (m_currNumTris < m_minNumLightsForPresampling)
        return;

    m_doPresamplingThisFrame = true;

    if (!m_sampleSets.IsInitialized())
    {
        Assert(m_numSampleSets > 0 && m_sampleSetSize > 0, "Rresampling params haven't been set.");
        const uint32_t sizeInBytes = m_numSampleSets * m_sampleSetSize * 
            sizeof(RT::PresampledEmissiveTriangle);

        m_sampleSets = GpuMemory::GetDefaultHeapBuffer("EmissiveSampleSets",
            sizeInBytes,
            D3D12_RESOURCE_STATE_COMMON,
            true);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::PRESAMPLED_EMISSIVE_SETS, 
            m_sampleSets);
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

        computeCmdList.PIXBeginEvent("EstimateTriLumen");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "EstimateTriLumen");

        computeCmdList.ResourceBarrier(m_lumen.Resource(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_rootSig.SetRootSRV(4, m_halton.GpuVA());
        m_rootSig.SetRootUAV(5, m_lumen.GpuVA());

        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::ESTIMATE_TRIANGLE_LUMEN));
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

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }

    if (m_doPresamplingThisFrame)
    {
        const uint32_t numSamples = m_numSampleSets * m_sampleSetSize;
        const uint32_t dispatchDimX = CeilUnsignedIntDiv(numSamples, PRESAMPLE_EMISSIVE_GROUP_DIM_X);
        Assert(dispatchDimX <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, "#blocks exceeded maximum allowed.");

        computeCmdList.PIXBeginEvent("PresampleEmissives");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "PresampleEmissives");

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
        cb.NumTotalSamples = numSamples;

        m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
        m_rootSig.SetRootUAV(5, m_sampleSets.GpuVA());
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::PRESAMPLING));
        computeCmdList.Dispatch(dispatchDimX, 1, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        cmdList.PIXEndEvent();
    }

    if (m_buildLVGThisFrame && m_lvg.IsInitialized())
    {
        computeCmdList.PIXBeginEvent("LightVoxelGrid");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "LightVoxelGrid");

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

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::BUILD_LIGHT_VOXEL_GRID));
        computeCmdList.Dispatch(m_voxelGridDim.x, m_voxelGridDim.y, m_voxelGridDim.z);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        cmdList.PIXEndEvent();
    }
}

void PreLighting::ToggleLVG()
{
    if (m_useLVG)
    {
        Assert(!m_lvg.IsInitialized(), "Redundant call.");
        const size_t sizeInBytes = NUM_SAMPLES_PER_VOXEL * m_voxelGridDim.x * m_voxelGridDim.y * m_voxelGridDim.z *
            sizeof(RT::VoxelSample);

        m_lvg = GpuMemory::GetDefaultHeapBuffer("LVG",
            (uint32_t)sizeInBytes,
            D3D12_RESOURCE_STATE_COMMON,
            true);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::LIGHT_VOXEL_GRID, m_lvg);



        App::AddShaderReloadHandler("BuildLightVoxelGrid", fastdelegate::MakeDelegate(this, &PreLighting::ReloadBuildLVG));
    }
    else
    {
        Assert(m_lvg.IsInitialized(), "Redundant call.");

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.RemoveDefaultHeapBuffer(GlobalResource::LIGHT_VOXEL_GRID, m_lvg);

        App::RemoveParam("Renderer", "Light Voxel Grid", "Extents");
        App::RemoveParam("Renderer", "Light Voxel Grid", "Y Offset");

        App::RemoveShaderReloadHandler("BuildLightVoxelGrid");
        m_lvg.Reset();
    }
}

void PreLighting::ReleaseLumenBufferAndReadback()
{
    m_lumen.Reset();
    m_readback.Reset();
    m_buildLVGThisFrame = m_useLVG;
}

void PreLighting::ReloadBuildLVG()
{
    const int i = (int)SHADER::BUILD_LIGHT_VOXEL_GRID;
    m_psoLib.Reload(i, m_rootSigObj.Get(), "PreLighting\\BuildLightVoxelGrid.hlsl");
}

//--------------------------------------------------------------------------------------
// EmissiveTriangleAliasTable
//--------------------------------------------------------------------------------------

void EmissiveTriangleAliasTable::Update(ReadbackHeapBuffer* readback)
{
    Assert(readback, "Readback buffer was NULL.");
    m_readback = readback;

    const size_t currBuffLen = m_aliasTable.IsInitialized() ? 
        m_aliasTable.Desc().Width / sizeof(float) : 0;
    m_currNumTris = (uint32_t)App::GetScene().NumEmissiveTriangles();
    Assert(m_currNumTris, "redundant call.");

    if (currBuffLen < m_currNumTris)
    {
        m_aliasTable = GpuMemory::GetDefaultHeapBuffer("AliasTable",
            m_currNumTris * sizeof(RT::EmissiveLumenAliasTableEntry),
            D3D12_RESOURCE_STATE_COMMON,
            false);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(
            GlobalResource::EMISSIVE_TRIANGLE_ALIAS_TABLE, m_aliasTable);
    }
}

void EmissiveTriangleAliasTable::SetEmissiveTriPassHandle(RenderNodeHandle& emissiveTriHandle)
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

    m_fence = m_fence == UINT64_MAX ?
        App::GetScene().GetRenderGraph()->GetCompletionFence(RenderNodeHandle(m_emissiveTriHandle)) :
        m_fence;
    Assert(m_fence != UINT64_MAX, "Invalid fence value.");

    // For 1st frame, wait until GPU finishes copying data to readback buffer. For subsequent
    // frames, check the fence and defer to next frame if not ready.
    if(App::GetTimer().GetTotalFrameCount() <= 1)
        renderer.WaitForDirectQueueFenceCPU(m_fence);
    else if (!renderer.IsDirectQueueFenceComplete(m_fence))
    {
        LOG_UI_INFO("Alias table - fence hasn't passed, returning...");
        return;
    }

    // Try to use frame allocator first, if it fails (allocation size exceeded per-frame max),
    // use malloc instead
    SmallVector<RT::EmissiveLumenAliasTableEntry, App::OneTimeFrameAllocatorWithFallback> table;
    table.resize(m_currNumTris);

    App::DeltaTimer timer;
    timer.Start();

    {
        // Safe to map, related fence has passed
        m_readback->Map();

        float* data = reinterpret_cast<float*>(m_readback->MappedMemory());
        BuildAliasTable(MutableSpan(data, m_currNumTris), table);

        // Unmapping happens automatically when readback buffer is released
        //m_readback->Unmap();
    }

    timer.End();
    LOG_UI_INFO("Alias table - computation took %u [us].", (uint32_t)timer.DeltaMicro());

    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "UploadAliasTable");
    computeCmdList.PIXBeginEvent("UploadAliasTable");

    // Schedule a copy
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
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    gpuTimer.EndQuery(computeCmdList, queryIdx);
    cmdList.PIXEndEvent();

    // Even though at this point this command list hasn't been submitted yet (only recorded),
    // it's safe to release the buffers here -- this is because resource deallocation
    // and signalling the related fence happens at the end of frame when all command 
    // lists have been submitted
    m_releaseDlg();
    m_fence = UINT64_MAX;
}
