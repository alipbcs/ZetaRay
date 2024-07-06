#include "RtAccelerationStructure.h"
#include "../App/Timer.h"
#include "../Core/RendererCore.h"
#include "../Core/CommandList.h"
#include "../Scene/SceneCore.h"
#include "../Math/MatrixFuncs.h"
#include "../Core/SharedShaderResources.h"
#include "../Math/Color.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RT;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Model;
using namespace ZetaRay::Model::glTF::Asset;

namespace
{
    ZetaInline D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS GetBuildFlagsForRtAS(RT_MESH_MODE t)
    {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS f = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

        if (t == RT_MESH_MODE::STATIC)
        {
            f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        }
        else if (t == RT_MESH_MODE::DYNAMIC_NO_REBUILD)
            f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        //else if (t == RT_MESH_MODE::DYNAMIC_REBUILD)
        //    f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

        return f;
    }
}

//--------------------------------------------------------------------------------------
// StaticBLAS
//--------------------------------------------------------------------------------------

void StaticBLAS::Rebuild(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();
    if (scene.m_numStaticInstances == 0)
        return;

    //Assert(scene.m_numOpaqueInstances + scene.m_numNonOpaqueInstances == scene.m_numStaticInstances, "these should match.");

    SmallVector<D3D12_RAYTRACING_GEOMETRY_DESC, App::FrameAllocator> meshDescs;
    meshDescs.resize(scene.m_numStaticInstances);

    constexpr int transfromMatSize = sizeof(BLASTransform);
    const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
    const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();
    const D3D12_GPU_VIRTUAL_ADDRESS transformGpuVa = m_perMeshTransform.GpuVA();
    int currInstance = 0;

    // Add a triangle mesh to list of geometries included in BLAS
    // Following loops should exactly match the ones in FillMeshTransformBufferForBuild()
    for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

        for (int i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
        {
            const Scene::RT_Flags flags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[i]);

            if (flags.MeshMode == RT_MESH_MODE::STATIC)
            {
                const uint64_t meshID = currTreeLevel.m_meshIDs[i];
                if (meshID == Scene::INVALID_MESH)
                    continue;

                const TriangleMesh* mesh = scene.GetMesh(meshID).value();

                meshDescs[currInstance].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                // Force mesh to be opaque when possible to avoid invoking any-hit shaders
                meshDescs[currInstance].Flags = flags.IsOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : 
                    D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                // Elements are tightly packed as size of each element is a multiple of required alignment
                meshDescs[currInstance].Triangles.Transform3x4 = transformGpuVa + currInstance * transfromMatSize;
                meshDescs[currInstance].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                meshDescs[currInstance].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                meshDescs[currInstance].Triangles.IndexCount = mesh->m_numIndices;
                meshDescs[currInstance].Triangles.VertexCount = mesh->m_numVertices;
                meshDescs[currInstance].Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
                meshDescs[currInstance].Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh->m_vtxBuffStartOffset * sizeof(Vertex);
                meshDescs[currInstance].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

                currInstance++;
            }
        }
    }

    Assert((uint32_t)currInstance == scene.m_numStaticInstances, "Invalid number of instances.");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
    buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::STATIC);
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = (UINT)meshDescs.size();
    buildDesc.Inputs.pGeometryDescs = meshDescs.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
    App::GetRenderer().GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuild);

    Assert(prebuild.ResultDataMaxSizeInBytes > 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");
    Assert(prebuild.ResultDataMaxSizeInBytes < UINT32_MAX, "Allocation size exceeded maximum allowed.");

    // Allocate a new buffer if this is the first time or the old one isn't large enough
    if (!m_buffer.IsInitialized() || m_buffer.Desc().Width < prebuild.ResultDataMaxSizeInBytes)
    {
        m_buffer = GpuMemory::GetDefaultHeapBuffer("StaticBLAS",
            (uint32_t)prebuild.ResultDataMaxSizeInBytes,
            true,
            true);
    }

    // Use the same buffer for scratch and compaction info
    m_compactionInfoStartOffset = (uint32_t)Math::AlignUp(prebuild.ScratchDataSizeInBytes,
        alignof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));
    const uint32_t scratchBuffSizeInBytes = m_compactionInfoStartOffset +
        sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);

    m_scratch = GpuMemory::GetDefaultHeapBuffer("StaticBLAS_scratch",
        scratchBuffSizeInBytes,
        D3D12_RESOURCE_STATE_COMMON,
        true);

    // For reading back the compacted size
    m_postBuildInfoReadback = GpuMemory::GetReadbackHeapBuffer(
        sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

    buildDesc.DestAccelerationStructureData = m_buffer.GpuVA();
    buildDesc.ScratchAccelerationStructureData = m_scratch.GpuVA();
    buildDesc.SourceAccelerationStructureData = 0;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compactionDesc;
    compactionDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    compactionDesc.DestBuffer = m_scratch.GpuVA() + m_compactionInfoStartOffset;

    cmdList.PIXBeginEvent("StaticBLASBuild");

    cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 1, &compactionDesc);

    // Wait until call above is completed before copying the compacted size
    auto barrier = Direct3DUtil::BufferBarrier(m_scratch.Resource(),
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_ACCESS_COPY_SOURCE);

    cmdList.ResourceBarrier(barrier);

    cmdList.CopyBufferRegion(m_postBuildInfoReadback.Resource(),            // dest
        0,                                                                  // dest offset
        m_scratch.Resource(),                                               // source
        m_compactionInfoStartOffset,                                        // source offset
        sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

    cmdList.PIXEndEvent();
}

void StaticBLAS::FillMeshTransformBufferForBuild()
{
    SceneCore& scene = App::GetScene();

    SmallVector<BLASTransform, App::FrameAllocator> transforms;
    transforms.resize(scene.m_numStaticInstances);

    int currInstance = 0;

    // Skip the first level
    for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

        for (int i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
        {
            if (currTreeLevel.m_meshIDs[i] == Scene::INVALID_MESH)
                continue;

            const auto rtFlags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[i]);

            if (rtFlags.MeshMode == RT_MESH_MODE::STATIC)
            {
                float4x3& M = currTreeLevel.m_toWorlds[i];

                for (int j = 0; j < 4; j++)
                {
                    transforms[currInstance].M[0][j] = M.m[j].x;
                    transforms[currInstance].M[1][j] = M.m[j].y;
                    transforms[currInstance].M[2][j] = M.m[j].z;
                }

                currInstance++;
            }
        }
    }

    m_perMeshTransform = GpuMemory::GetDefaultHeapBufferAndInit("StaticBLASTransform",
        sizeof(BLASTransform) * scene.m_numStaticInstances, false, transforms.data());
}

void StaticBLAS::DoCompaction(ComputeCmdList& cmdList)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC compactDesc;

    m_postBuildInfoReadback.Map();
    memcpy(&compactDesc, m_postBuildInfoReadback.MappedMemory(), sizeof(compactDesc));
    m_postBuildInfoReadback.Unmap();

    // Scratch buffer is not needed anymore
    m_scratch.Reset();

    Check(compactDesc.CompactedSizeInBytes > 0, "Invalid RtAS compacted size.");

    // Allocate a new BLAS with compacted size
    m_bufferCompacted = GpuMemory::GetDefaultHeapBuffer("StaticBLASCompacted",
        (uint32_t)compactDesc.CompactedSizeInBytes,
        true,
        true);

    cmdList.PIXBeginEvent("StaticBLAS::Compaction");

    cmdList.CompactAccelerationStructure(m_bufferCompacted.GpuVA(), m_buffer.GpuVA());

#if 0
    auto barrier = Direct3DUtil::BufferBarrier(m_bufferCompacted.Resource(),
        D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_BARRIER_SYNC_RAYTRACING,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

    cmdList.ResourceBarrier(barrier);
#endif

    cmdList.PIXEndEvent();
}

void StaticBLAS::CompactionCompletedCallback()
{
    m_buffer = ZetaMove(m_bufferCompacted);
    m_postBuildInfoReadback.Reset();
    m_perMeshTransform.Reset();
}

void StaticBLAS::Clear()
{
    m_buffer.Reset();
    m_bufferCompacted.Reset();
    m_perMeshTransform.Reset();
    m_scratch.Reset();
}

//--------------------------------------------------------------------------------------
// DynamicBLAS
//--------------------------------------------------------------------------------------

DynamicBlasBuildItem DynamicBLAS::Rebuild()
{
    DynamicBlasBuildItem ret;

    SceneCore& scene = App::GetScene();
    const TriangleMesh* mesh = scene.GetMesh(m_meshID).value();

    const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
    const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();

    ret.GeoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    ret.GeoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    ret.GeoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
    ret.GeoDesc.Triangles.IndexCount = mesh->m_numIndices;
    ret.GeoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    ret.GeoDesc.Triangles.Transform3x4 = 0;
    ret.GeoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh->m_vtxBuffStartOffset * sizeof(Vertex);
    ret.GeoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    ret.GeoDesc.Triangles.VertexCount = mesh->m_numVertices;
    ret.GeoDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

    ret.BlasBufferOffset = &m_blasBufferOffset;

    return ret;
}

//--------------------------------------------------------------------------------------
// TLAS
//--------------------------------------------------------------------------------------

void TLAS::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& gpuTimer = App::GetRenderer().GetGpuTimer();
    computeCmdList.PIXBeginEvent("RtAS");
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "RtAS");

    RebuildOrUpdateBLASes(computeCmdList);
    RebuildTLASInstances(computeCmdList);
    RebuildTLAS(computeCmdList);

    gpuTimer.EndQuery(computeCmdList, queryIdx);
    computeCmdList.PIXEndEvent();
}

void TLAS::RebuildTLASInstances(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    const int numInstances = (int)m_dynamicBLASes.size() + (scene.m_numStaticInstances > 0);
    if (numInstances == 0)
        return;

    SmallVector<D3D12_RAYTRACING_INSTANCE_DESC, App::FrameAllocator, 1> tlasInstances;
    tlasInstances.resize(numInstances);

    const bool hasStaticInstances = scene.m_numStaticInstances > 0;

    if (hasStaticInstances)
    {
        tlasInstances[0] = D3D12_RAYTRACING_INSTANCE_DESC{
                .InstanceID = 0,
                .InstanceMask = RT_AS_SUBGROUP::ALL,
                .InstanceContributionToHitGroupIndex = 0,
                .Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE,
                .AccelerationStructure = m_staticBLAS.m_buffer.GpuVA()
            };

        // Identity transform for static BLAS instance
        memset(&tlasInstances[0].Transform, 0, sizeof(BLASTransform));
        tlasInstances[0].Transform[0][0] = 1.0f;
        tlasInstances[0].Transform[1][1] = 1.0f;
        tlasInstances[0].Transform[2][2] = 1.0f;
    }

    const int numStaticInstances = scene.m_numStaticInstances;

    // Following traversal order must match the one in RebuildOrUpdateBLASes()
    D3D12_RAYTRACING_INSTANCE_DESC instance;
    int currDynamicInstance = 0;
    const auto dynamicBlasGpuVa = m_dynamicBlasBuffer.IsInitialized() ? m_dynamicBlasBuffer.GpuVA() : 0;

    // Skip the first level
    for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
        const auto& rtFlagVec = currTreeLevel.m_rtFlags;

        // Add one TLAS instance for every dynamic mesh
        for (int i = 0; i < rtFlagVec.size(); i++)
        {
            if (currTreeLevel.m_meshIDs[i] == Scene::INVALID_MESH)
                continue;

            const auto flags = Scene::GetRtFlags(rtFlagVec[i]);

            if (flags.MeshMode != RT_MESH_MODE::STATIC)
            {
                instance.InstanceID = numStaticInstances + currDynamicInstance;
                instance.InstanceMask = flags.InstanceMask;
                instance.InstanceContributionToHitGroupIndex = 0;
                instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                instance.AccelerationStructure = dynamicBlasGpuVa + m_dynamicBLASes[currDynamicInstance].m_blasBufferOffset;

                auto& M = currTreeLevel.m_toWorlds[i];

                for (int j = 0; j < 4; j++)
                {
                    instance.Transform[0][j] = M.m[j].x;
                    instance.Transform[1][j] = M.m[j].y;
                    instance.Transform[2][j] = M.m[j].z;
                }

                currTreeLevel.m_rtASInfo[i].InstanceID = instance.InstanceID;

                tlasInstances[hasStaticInstances + currDynamicInstance++] = instance;
            }
        }
    }

    Assert(hasStaticInstances + currDynamicInstance == numInstances, "bug");
    const uint32_t sizeInBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances;

    if (!m_tlasInstanceBuff.IsInitialized() || m_tlasInstanceBuff.Desc().Width < sizeInBytes)
    {
        m_tlasInstanceBuff = GpuMemory::GetDefaultHeapBuffer("TLASInstances",
            sizeInBytes, D3D12_RESOURCE_STATE_COMMON, false);
    }

    UploadHeapBuffer scratchBuff = GpuMemory::GetUploadHeapBuffer(sizeInBytes);
    scratchBuff.Copy(0, sizeInBytes, tlasInstances.data());

    cmdList.CopyBufferRegion(m_tlasInstanceBuff.Resource(),
        0,
        scratchBuff.Resource(),
        scratchBuff.Offset(),
        sizeInBytes);

    // Wait for copy to be finished before doing compute work
    auto barrier = Direct3DUtil::BufferBarrier(m_tlasInstanceBuff.Resource(),
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_COPY_DEST,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

    cmdList.ResourceBarrier(barrier);
}

void TLAS::RebuildTLAS(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    const int numInstances = (int)m_dynamicBLASes.size() + (scene.m_numStaticInstances > 0);
    if (numInstances == 0)
        return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
    buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = numInstances;
    buildDesc.Inputs.InstanceDescs = m_tlasInstanceBuff.GpuVA();

    auto* device = App::GetRenderer().GetDevice();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuildInfo);
    Assert(prebuildInfo.ResultDataMaxSizeInBytes != 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

    if (!m_tlasBuffer.IsInitialized() || m_tlasBuffer.Desc().Width < prebuildInfo.ResultDataMaxSizeInBytes)
    {
        // previous TLAS is released with proper fence
        m_tlasBuffer = GpuMemory::GetDefaultHeapBuffer("TLAS",
            (uint32_t)prebuildInfo.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            true);

        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_SCENE_BVH, m_tlasBuffer);
    }

    if (!m_scratchBuffer.IsInitialized() || m_scratchBuffer.Desc().Width < prebuildInfo.ScratchDataSizeInBytes)
    {
        m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("TLAS_scratch",
            (uint32_t)prebuildInfo.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_COMMON,
            true);
    }

    buildDesc.DestAccelerationStructureData = m_tlasBuffer.GpuVA();
    // Note that scratch buffer is reused for dynamic BLAS builds & TLAS with overlapping
    // addresses, but due to inserted barrier, it's safe.
    buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA();
    buildDesc.SourceAccelerationStructureData = 0;

    cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // Even though TLAS was created with an initial stete of D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
    // the debug layer warns that "D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ and 
    // D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE can only be used with resources 
    // created using D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE or with a legacy InitialState 
    // of D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE".
#if 0
    // wait for build to be finished before doing any raytracing
    D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_tlasBuffer.Resource(),
        D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

    cmdList.ResourceBarrier(barrier);
#endif

    m_ready = true;
}

void TLAS::RebuildOrUpdateBLASes(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    // From DXR specs:
    // acceleration structures must always be in D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
    // so resource state transitions can't be used to synchronize between writes and reads of acceleration 
    // structure data. Instead, UAV barriers must be used on resources holding acceleration structure data 
    // between operations that write to an acceleration structure (such as BuildRaytracingAccelerationStructure()) 
    // and operations that read from them (such as DispatchRays())

    // From Ray Tracing Gems Chapter 19:
    // "One important optimization is to ensure that any resource transition barriers that
    // are needed after BLAS updates are deferred to be executed right before the
    // TLAS build, instead of executing these right after each BLAS update. Deferral is
    // important because each of these transition barriers is a synchronization step on
    // the GPU. Having the transitions coalesced into a single point in the command buffer
    // avoids redundant synchronization that would otherwise cause the GPU to frequently
    // become idle."
    SmallVector<D3D12_BUFFER_BARRIER, App::FrameAllocator> uavBarriers;

    if (scene.m_hasNewStaticInstances)
    {
        m_staticBLASrebuiltFrame = (uint32_t)App::GetTimer().GetTotalFrameCount();
        m_staticBLAS.Rebuild(cmdList);

        D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_staticBLAS.m_buffer.Resource(),
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
            D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

        uavBarriers.push_back(barrier);

        scene.m_hasNewStaticInstances = false;
    }
    // TODO use a fence instead of assuming the worst case
    else if (scene.m_numStaticInstances > 0 && m_staticBLASrebuiltFrame == App::GetTimer().GetTotalFrameCount() - Constants::NUM_BACK_BUFFERS)
    {
        m_staticBLAS.DoCompaction(cmdList);
    }
    else if (scene.m_numStaticInstances > 0 && m_staticBLASrebuiltFrame == App::GetTimer().GetTotalFrameCount() - Constants::NUM_BACK_BUFFERS - 1)
        m_staticBLAS.CompactionCompletedCallback();

    if (scene.m_hasNewDynamicInstances)
    {
        RebuildDynamicBLASes(cmdList);

        // One barrier covers all of the builds
        D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_dynamicBlasBuffer.Resource(),
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
            D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

        uavBarriers.push_back(barrier);

        scene.m_hasNewDynamicInstances = false;
    }

    if (!uavBarriers.empty())
        cmdList.ResourceBarrier(uavBarriers.data(), (uint32_t)uavBarriers.size());
}

void TLAS::RebuildDynamicBLASes(Core::ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    SmallVector<DynamicBlasBuildItem, App::FrameAllocator> blasBuilds;
    m_dynamicBLASes.clear();

    // Skip the first level
    for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
        auto& rtFlagVec = currTreeLevel.m_rtFlags;

        for (int i = 0; i < rtFlagVec.size(); i++)
        {
            Scene::RT_Flags flags = Scene::GetRtFlags(rtFlagVec[i]);
            Assert((flags.RebuildFlag & flags.UpdateFlag) == 0, "Rebuild & update flags can't be set at the same time.");

            if (flags.MeshMode != RT_MESH_MODE::STATIC)
            {
                m_dynamicBLASes.emplace_back(currTreeLevel.m_IDs[i], currTreeLevel.m_meshIDs[i]);

                auto buildItem = m_dynamicBLASes.back().Rebuild();
                blasBuilds.push_back(buildItem);

                rtFlagVec[i] = Scene::SetRtFlags(flags.MeshMode, flags.InstanceMask, 0, 0, flags.IsOpaque);
            }
        }
    }

    if (!blasBuilds.empty())
    {
        auto* device = App::GetRenderer().GetDevice();
        uint32_t currBuildSizeInBytes = 0;
        uint32_t currScratchSize = 0;

        for (auto& b : blasBuilds)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
            buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::DYNAMIC_NO_REBUILD);
            buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            buildDesc.Inputs.NumDescs = 1;
            buildDesc.Inputs.pGeometryDescs = &b.GeoDesc;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
            device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuild);

            Assert(prebuild.ResultDataMaxSizeInBytes > 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

            currBuildSizeInBytes = Math::AlignUp(currBuildSizeInBytes, uint32_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
            *b.BlasBufferOffset = currBuildSizeInBytes;
            currBuildSizeInBytes += (uint32_t)prebuild.ResultDataMaxSizeInBytes;

            currScratchSize = Math::AlignUp(currScratchSize, uint32_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
            b.ScratchBufferOffset = currScratchSize;
            currScratchSize += (uint32_t)prebuild.ScratchDataSizeInBytes;
        }

        m_dynamicBlasBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS",
            currBuildSizeInBytes,
            true,
            true);

        if (!m_scratchBuffer.IsInitialized() || m_scratchBuffer.Desc().Width < currScratchSize)
        {
            m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS_scratch",
                currScratchSize,
                D3D12_RESOURCE_STATE_COMMON,
                true);
        }

        for (auto& b : blasBuilds)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
            buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::DYNAMIC_NO_REBUILD);
            buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            buildDesc.Inputs.NumDescs = 1;
            buildDesc.Inputs.pGeometryDescs = &b.GeoDesc;

            buildDesc.DestAccelerationStructureData = m_dynamicBlasBuffer.GpuVA() + *b.BlasBufferOffset;
            buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA() + b.ScratchBufferOffset;
            buildDesc.SourceAccelerationStructureData = 0;

            cmdList.PIXBeginEvent("DynamicBLASBuild");
            cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
            cmdList.PIXEndEvent();
        }
    }
}

void TLAS::BuildFrameMeshInstanceData()
{
    SceneCore& scene = App::GetScene();
    const uint32_t numInstances = (uint32_t)scene.m_IDtoTreePos.size();
    m_frameInstanceData.resize(numInstances);

    uint32_t currInstance = 0;
    const bool sceneHasEmissives = scene.NumEmissiveInstances() > 0;

    auto addTLASInstance = [&scene, this, &currInstance, sceneHasEmissives](const SceneCore::TreeLevel& currTreeLevel,
        int levelIdx, bool staticMesh)
    {
        if (currTreeLevel.m_meshIDs[levelIdx] == Scene::INVALID_MESH)
            return;

        const auto rtFlags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[levelIdx]);

        if (staticMesh && rtFlags.MeshMode != RT_MESH_MODE::STATIC)
            return;

        if (!staticMesh && rtFlags.MeshMode == RT_MESH_MODE::STATIC)
            return;

        const TriangleMesh* mesh = scene.GetMesh(currTreeLevel.m_meshIDs[levelIdx]).value();
        const Material* mat = scene.GetMaterial(mesh->m_materialIdx).value();

        const EmissiveInstance* emissiveInstance = sceneHasEmissives && (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE) ?
            scene.m_emissives.FindEmissive(currTreeLevel.m_IDs[levelIdx]).value() :
            nullptr;

        auto& M = currTreeLevel.m_toWorlds[levelIdx];
        v_float4x4 vM = load4x3(M);

        // Meshes in TLAS go through the following transformations:
        // 
        // 1. Optional transform during BLAS build
        // 2. Per-instance transform for each BLAS instance in TLAS
        //
        // When accessing triangle data in closest-hit shaders, transform 2 can be accessed
        // using the ObjectToWorld3x4() intrinsic, but transform 1 is lost
        float4a t;
        float4a r;
        float4a s;
        decomposeSRT(vM, s, r, t);

        m_frameInstanceData[currInstance].MatIdx = (uint16_t)mat->GpuBufferIndex();
        m_frameInstanceData[currInstance].BaseVtxOffset = (uint32_t)mesh->m_vtxBuffStartOffset;
        m_frameInstanceData[currInstance].BaseIdxOffset = (uint32_t)mesh->m_idxBuffStartOffset;
        m_frameInstanceData[currInstance].Rotation = snorm4(r);
        m_frameInstanceData[currInstance].Scale = half3(s);
        m_frameInstanceData[currInstance].Translation = float3(t.x, t.y, t.z);
        m_frameInstanceData[currInstance].BaseEmissiveTriOffset = emissiveInstance ? emissiveInstance->BaseTriOffset : UINT32_MAX;
        m_frameInstanceData[currInstance].BaseColorTex = mat->BaseColorTexture == UINT32_MAX
            ? UINT16_MAX :
            (uint16_t)mat->BaseColorTexture;

        float alpha = float((mat->BaseColorFactor >> 24) & 0xff) / 255.0f;
        m_frameInstanceData[currInstance].AlphaFactor_Cuttoff = Float2ToRG8(float2(alpha, mat->GetAlphaCuttoff()));

        const float4x3& M_prev = *scene.GetPrevToWorld(currTreeLevel.m_IDs[levelIdx]).value();
        v_float4x4 vM_prev = load4x3(M_prev);
        float4a t_prev;
        float4a r_prev;
        float4a s_prev;
        decomposeSRT(vM_prev, s_prev, r_prev, t_prev);

        m_frameInstanceData[currInstance].PrevRotation = snorm4(r_prev);
        m_frameInstanceData[currInstance].PrevScale = half3(s_prev);
        m_frameInstanceData[currInstance].dTranslation = half3(float3(t.x - t_prev.x, t.y - t_prev.y, t.z - t_prev.z));

        currInstance++;
    };

    // Layout:
    // 
    //  - N static meshes (SM)
    //  - D dynamic meshes (DM)
    //  -------------------------------------------------------------
    // | SM 0 | SM 1 | ... | SM N - 1 | DM 0 | DM 1 | ... | DM D - 1 |
    //  -------------------------------------------------------------
    // 
    // TLAS instance for Static BLAS has instanceID of 0. 
    // TLAS instance for Dynamic BLAS d where 0 <= d < D has InstanceID of N + d
    // With this setup, every instance can use GeometryIndex() + InstanceID() to index into the mesh instance buffer

    const bool rebuildStatic = App::GetScene().m_hasNewStaticInstances;

    // static meshes
    if (rebuildStatic)
    {
        for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
        {
            auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
            const auto& rtFlagVec = currTreeLevel.m_rtFlags;

            for (int i = 0; i < rtFlagVec.size(); i++)
                addTLASInstance(currTreeLevel, i, true);
        }
    }

    Assert(!rebuildStatic || currInstance == scene.m_numStaticInstances, "bug");
    currInstance = scene.m_numStaticInstances;

    // Dynamic meshes
    for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
        const auto& rtFlagVec = currTreeLevel.m_rtFlags;

        for (int i = 0; i < rtFlagVec.size(); i++)
            addTLASInstance(currTreeLevel, i, false);
    }

    const uint32_t sizeInBytes = numInstances * sizeof(RT::MeshInstance);
    auto& renderer = App::GetRenderer();

    if (!m_framesMeshInstances.IsInitialized() || m_framesMeshInstances.Desc().Width < sizeInBytes)
    {
        m_framesMeshInstances = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::RT_FRAME_MESH_INSTANCES,
            sizeInBytes,
            false,
            m_frameInstanceData.data());

        // Register the shared resource
        auto& r = App::GetRenderer().GetSharedShaderResources();
        r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES, m_framesMeshInstances);
    }
    else
        // This is recorded now but submitted after last frame's submissions
        GpuMemory::UploadToDefaultHeapBuffer(m_framesMeshInstances, sizeInBytes, m_frameInstanceData.data());
}

void TLAS::BuildStaticBLASTransforms()
{
    SceneCore& scene = App::GetScene();

    if (scene.m_hasNewStaticInstances)
        m_staticBLAS.FillMeshTransformBufferForBuild();
}
