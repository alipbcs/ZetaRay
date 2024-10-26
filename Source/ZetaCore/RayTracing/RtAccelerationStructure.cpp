#include "RtAccelerationStructure.h"
#include "../Core/RendererCore.h"
#include "../Core/CommandList.h"
#include "../Scene/SceneCore.h"
#include "../Core/SharedShaderResources.h"
#include "../Core/RenderGraph.h"
#include "../App/Log.h"
#include "../App/Timer.h"
#include <algorithm>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RT;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Model;
using namespace ZetaRay::Model::glTF::Asset;
using namespace ZetaRay::Support;

namespace
{
    struct BLASTransform
    {
        float M[3][4];
    };

    struct DynamicBlasBuild
    {
        D3D12_RAYTRACING_GEOMETRY_DESC GeoDesc;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO BuildInfo;
        uint32_t BlasBufferOffset;
        uint32_t ScratchBufferOffset;
        uint32_t TreeLevel;
        uint32_t LevelIdx;
    };

    ZetaInline D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS BuildFlags(RT_MESH_MODE t)
    {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS f = 
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

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

void StaticBLAS::FillMeshTransformBufferForBuild(ID3D12Heap* heap, uint32_t heapOffsetInBytes)
{
    SceneCore& scene = App::GetScene();
    Assert(scene.m_numStaticInstances, "Invalid call.");

    SmallVector<BLASTransform, App::FrameAllocator> transforms;
    transforms.resize(scene.m_numStaticInstances);

    size_t currInstance = 0;

    // Skip the first level
    for (size_t treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

        for (size_t i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
        {
            if (currTreeLevel.m_meshIDs[i] == Scene::INVALID_MESH)
                continue;

            const auto rtFlags = RT_Flags::Decode(currTreeLevel.m_rtFlags[i]);

            if (rtFlags.MeshMode == RT_MESH_MODE::STATIC)
            {
                const float4x3& M = currTreeLevel.m_toWorlds[i];

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

    Assert(!m_perMeshTransform.IsInitialized(), "Unexpected condition.");
    const uint32_t sizeInBytes = scene.m_numStaticInstances * sizeof(BLASTransform);

    if (heap)
    {
        m_perMeshTransform = GpuMemory::GetPlacedHeapBufferAndInit("StaticBLASTransform",
            sizeInBytes,
            heap,
            heapOffsetInBytes,
            false,
            MemoryRegion{ .Data = transforms.data(), .SizeInBytes = sizeInBytes });
    }
    // Not possible to batch memory allocations with other resources in first frame
    else
    {
        m_perMeshTransform = GpuMemory::GetDefaultHeapBufferAndInit("StaticBLASTransform",
            sizeInBytes, false, 
            MemoryRegion{ .Data = transforms.data(), .SizeInBytes = sizeInBytes });
    }
}

void StaticBLAS::Rebuild(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();
    //Assert(scene.m_numOpaqueInstances + scene.m_numNonOpaqueInstances == 
    // scene.m_numStaticInstances, "these should match.");

    SmallVector<D3D12_RAYTRACING_GEOMETRY_DESC, App::FrameAllocator> meshDescs;
    meshDescs.resize(scene.m_numStaticInstances);

    constexpr int transformMatSize = sizeof(BLASTransform);
    const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
    const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();
    const D3D12_GPU_VIRTUAL_ADDRESS transformGpuVa = m_perMeshTransform.GpuVA();
    size_t currInstance = 0;

    // Add a triangle mesh to list of BLAS geometries
    // Following loop should exactly match the one in FillMeshTransformBufferForBuild()
    for (size_t treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

        for (size_t i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
        {
            const RT_Flags flags = RT_Flags::Decode(currTreeLevel.m_rtFlags[i]);

            if (flags.MeshMode == RT_MESH_MODE::STATIC)
            {
                const uint64_t meshID = currTreeLevel.m_meshIDs[i];
                if (meshID == Scene::INVALID_MESH)
                    continue;

                const TriangleMesh* mesh = scene.GetMesh(meshID).value();

                meshDescs[currInstance].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                // Force mesh to be opaque when possible to avoid invoking any-hit shaders
                meshDescs[currInstance].Flags = flags.IsOpaque ? 
                    D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : 
                    D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                // Elements are tightly packed as size of each element is a multiple 
                // of required alignment
                meshDescs[currInstance].Triangles.Transform3x4 = transformGpuVa + 
                    currInstance * transformMatSize;
                meshDescs[currInstance].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                meshDescs[currInstance].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                meshDescs[currInstance].Triangles.IndexCount = mesh->m_numIndices;
                meshDescs[currInstance].Triangles.VertexCount = mesh->m_numVertices;
                meshDescs[currInstance].Triangles.IndexBuffer = sceneIBGpuVa + 
                    mesh->m_idxBuffStartOffset * sizeof(uint32_t);
                meshDescs[currInstance].Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + 
                    mesh->m_vtxBuffStartOffset * sizeof(Vertex);
                meshDescs[currInstance].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

                currTreeLevel.m_rtASInfo[i] = RT_AS_Info{
                    .GeometryIndex = (uint32_t)currInstance,
                    .InstanceID = 0 };

                currInstance++;
            }
        }
    }

    Assert(currInstance == scene.m_numStaticInstances, "Invalid instance index.");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
    buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    buildDesc.Inputs.Flags = BuildFlags(RT_MESH_MODE::STATIC);
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = (UINT)meshDescs.size();
    buildDesc.Inputs.pGeometryDescs = meshDescs.data();

    // Very expensive. Only needed for first frame - future frames use the cached result.
    if (m_prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        App::GetRenderer().GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(
            &buildDesc.Inputs, &m_prebuildInfo);
    }

    Assert(m_prebuildInfo.ResultDataMaxSizeInBytes > 0,
        "GetRaytracingAccelerationStructurePrebuildInfo() failed.");
    Assert(m_prebuildInfo.ResultDataMaxSizeInBytes < UINT32_MAX,
        "Allocation size exceeded maximum allowed.");

    // Use the same buffer for scratch and compaction info
    const uint32_t compactionInfoStartOffset = (uint32_t)AlignUp(m_prebuildInfo.ScratchDataSizeInBytes,
        alignof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));
    const uint32_t scratchBuffSizeInBytes = compactionInfoStartOffset +
        sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);

    if (!m_resHeap.IsInitialized())
    {
        PlacedResourceList<2> list;
        // BLAS (before compaction)
        list.PushBuffer((uint32_t)m_prebuildInfo.ResultDataMaxSizeInBytes, true, true);
        // Scratch buffer & compaction info
        list.PushBuffer(scratchBuffSizeInBytes, true, false);
        list.End();

        m_resHeap = GpuMemory::GetResourceHeap(list.TotalSizeInBytes());

        auto allocs = list.AllocInfos();
        m_BLASHeapOffsetInBytes = (uint32)allocs[0].Offset;
        m_scratchHeapOffsetInBytes = (uint32)allocs[1].Offset;
    }

    Assert(m_BLASHeapOffsetInBytes != UINT32_MAX, "Invalid heap offset.");
    Assert(m_scratchHeapOffsetInBytes != UINT32_MAX, "Invalid heap offset.");

    m_buffer = GpuMemory::GetPlacedHeapBuffer("StaticBLAS",
        (uint32_t)m_prebuildInfo.ResultDataMaxSizeInBytes,
        m_resHeap.Heap(),
        m_BLASHeapOffsetInBytes,
        true,
        true);
    m_scratch = GpuMemory::GetPlacedHeapBuffer("StaticBLAS_scratch",
        scratchBuffSizeInBytes,
        m_resHeap.Heap(),
        m_scratchHeapOffsetInBytes,
        true,
        false);

    // For reading back the compacted size
    m_postBuildInfoReadback = GpuMemory::GetReadbackHeapBuffer(
        sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

    buildDesc.DestAccelerationStructureData = m_buffer.GpuVA();
    buildDesc.ScratchAccelerationStructureData = m_scratch.GpuVA();
    buildDesc.SourceAccelerationStructureData = 0;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compactionDesc;
    compactionDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    compactionDesc.DestBuffer = m_scratch.GpuVA() + compactionInfoStartOffset;

    cmdList.PIXBeginEvent("StaticBLASBuild");

    cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 1, &compactionDesc);

    // Wait until build is completed before copying the compacted size
    auto barrier = Direct3DUtil::BufferBarrier(m_scratch.Resource(),
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_ACCESS_COPY_SOURCE);

    cmdList.ResourceBarrier(barrier);

    cmdList.CopyBufferRegion(m_postBuildInfoReadback.Resource(),
        0,
        m_scratch.Resource(),
        compactionInfoStartOffset,
        sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

    cmdList.PIXEndEvent();

    m_BLASHeapOffsetInBytes = UINT32_MAX;
    m_scratchHeapOffsetInBytes = UINT32_MAX;
}

void StaticBLAS::DoCompaction(ComputeCmdList& cmdList)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC compactDesc;

    m_postBuildInfoReadback.Map();
    memcpy(&compactDesc, m_postBuildInfoReadback.MappedMemory(), sizeof(compactDesc));

    Check(compactDesc.CompactedSizeInBytes > 0, "Invalid RtAS compacted size.");
    LOG_UI_INFO("Allocated compacted static BLAS (%llu MB -> %llu MB).", 
        m_prebuildInfo.ResultDataMaxSizeInBytes / (1024 * 1024),
        compactDesc.CompactedSizeInBytes / (1024 * 1024));

    // Allocate a new BLAS with compacted size
    m_bufferCompacted = GpuMemory::GetDefaultHeapBuffer("CompactStaticBLAS",
        (uint32_t)compactDesc.CompactedSizeInBytes,
        true,
        true);

    cmdList.PIXBeginEvent("StaticBLAS_Compaction");
    cmdList.CompactAccelerationStructure(m_bufferCompacted.GpuVA(), m_buffer.GpuVA());
    cmdList.PIXEndEvent();
}

void StaticBLAS::CompactionCompletedCallback()
{
    m_compactionCompleted.store(true, std::memory_order_release);
    m_heapAllocated.store(false, std::memory_order_relaxed);

    // Release resources that are not needed anymore
    m_postBuildInfoReadback.Unmap();
    m_scratch.Reset();
    m_postBuildInfoReadback.Reset();
    m_perMeshTransform.Reset();
    m_resHeap.Reset();
}

//--------------------------------------------------------------------------------------
// TLAS
//--------------------------------------------------------------------------------------

void TLAS::FillMeshInstanceData(uint64_t instanceID, uint64_t meshID, const float4x3& M,
    uint32_t emissiveTriOffset, bool staticMesh, uint32_t currInstance)
{
    Assert(meshID != Scene::INVALID_MESH, "Invalid call.");

    SceneCore& scene = App::GetScene();
    const TriangleMesh* mesh = scene.GetMesh(meshID).value();
    uint32 matBufferIdx = UINT32_MAX;
    const Material* mat = scene.GetMaterial(mesh->m_materialID, &matBufferIdx).value();

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

    m_frameInstanceData[currInstance].MatIdx = (uint16_t)matBufferIdx;
    m_frameInstanceData[currInstance].BaseVtxOffset = mesh->m_vtxBuffStartOffset;
    m_frameInstanceData[currInstance].BaseIdxOffset = mesh->m_idxBuffStartOffset;
    m_frameInstanceData[currInstance].Rotation = unorm4::FromNormalized(r);
    m_frameInstanceData[currInstance].Scale = half3(s);
    m_frameInstanceData[currInstance].Translation = float3(t.x, t.y, t.z);
    m_frameInstanceData[currInstance].BaseEmissiveTriOffset = emissiveTriOffset;

    const uint32_t texIdx = mat->GetBaseColorTex();
    m_frameInstanceData[currInstance].BaseColorTex = texIdx == Material::INVALID_ID ?
        UINT16_MAX :
        (uint16_t)texIdx;

    float alpha = float((mat->BaseColorFactor >> 24) & 0xff) / 255.0f;
    m_frameInstanceData[currInstance].AlphaFactor_Cutoff =
        Float2ToRG8(float2(alpha, mat->GetAlphaCutoff()));

    if (!staticMesh)
    {
        const float4x3& M_prev = *scene.GetPrevToWorld(instanceID).value();
        v_float4x4 vM_prev = load4x3(M_prev);
        float4a t_prev;
        float4a r_prev;
        float4a s_prev;
        decomposeSRT(vM_prev, s_prev, r_prev, t_prev);

        m_frameInstanceData[currInstance].PrevRotation = unorm4::FromNormalized(r_prev);
        m_frameInstanceData[currInstance].PrevScale = half3(s_prev);
        m_frameInstanceData[currInstance].dTranslation = half3(t - t_prev);
    }
    else
    {
        m_frameInstanceData[currInstance].PrevRotation =
            m_frameInstanceData[currInstance].Rotation;
        m_frameInstanceData[currInstance].PrevScale =
            m_frameInstanceData[currInstance].Scale;
        m_frameInstanceData[currInstance].dTranslation = half3(0, 0, 0);
    }
}

void TLAS::RebuildFrameMeshInstanceData()
{
    SceneCore& scene = App::GetScene();
    const uint32_t numInstances = scene.m_numStaticInstances + scene.m_numDynamicInstances;
    m_frameInstanceData.resize(numInstances);

    size_t currInstance = 0;
    const bool sceneHasEmissives = scene.NumEmissiveInstances() > 0;

    // Resize to avoid repeatedly growing it
    scene.m_rtMeshInstanceIdxToID.resize(numInstances);

    // Layout:
    // 
    //  - N static meshes (SM)
    //  - D dynamic meshes (DM)
    //  -------------------------------------------------------------
    // | SM 0 | SM 1 | ... | SM N - 1 | DM 0 | DM 1 | ... | DM D - 1 |
    //  -------------------------------------------------------------
    // 
    //  - TLAS instance for static BLAS has instanceID of 0. 
    //  - TLAS instance for dynamic BLAS d where 0 <= d < D has InstanceID of N + d
    //  - With this setup, every instance can use GeometryIndex() + InstanceID() to index 
    //    into the mesh instance buffer

    // Static meshes
    if (scene.m_numStaticInstances)
    {
        for (size_t treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
        {
            auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
            const auto& rtFlagVec = currTreeLevel.m_rtFlags;

            for (size_t i = 0; i < rtFlagVec.size(); i++)
            {
                const auto rtFlags = RT_Flags::Decode(currTreeLevel.m_rtFlags[i]);
                const uint64_t meshID = currTreeLevel.m_meshIDs[i];
                if (meshID == Scene::INVALID_MESH)
                    continue;

                if (rtFlags.MeshMode == RT_MESH_MODE::STATIC)
                {
                    const uint64_t instanceID = currTreeLevel.m_IDs[i];
                    const uint32_t emissiveTriOffset = sceneHasEmissives &&
                        (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE) ?
                        scene.m_emissives.FindInstance(instanceID).value()->BaseTriOffset :
                        UINT32_MAX;

                    FillMeshInstanceData(instanceID, meshID, currTreeLevel.m_toWorlds[i], 
                        emissiveTriOffset, true, (uint32)currInstance);

                    // Update RT mesh to instance ID map
                    scene.m_rtMeshInstanceIdxToID[currInstance++] = currTreeLevel.m_IDs[i];
                }
            }
        }
    }

    Assert(currInstance == scene.m_numStaticInstances, "Invalid instance count.");

    // Dynamic meshes
    if (scene.m_numDynamicInstances)
    {
        for (size_t treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
        {
            auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
            const auto& rtFlagVec = currTreeLevel.m_rtFlags;

            for (size_t i = 0; i < rtFlagVec.size(); i++)
            {
                const auto rtFlags = RT_Flags::Decode(currTreeLevel.m_rtFlags[i]);
                const uint64_t meshID = currTreeLevel.m_meshIDs[i];
                if (meshID == Scene::INVALID_MESH)
                    continue;

                if (rtFlags.MeshMode == RT_MESH_MODE::DYNAMIC_NO_REBUILD)
                {
                    const uint64_t instanceID = currTreeLevel.m_IDs[i];
                    const uint32_t emissiveTriOffset = sceneHasEmissives &&
                        (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE) ?
                        scene.m_emissives.FindInstance(instanceID).value()->BaseTriOffset :
                        UINT32_MAX;

                    FillMeshInstanceData(instanceID, meshID, currTreeLevel.m_toWorlds[i], 
                        emissiveTriOffset, false, (uint32)currInstance);

                    // Update RT mesh to instance ID map
                    scene.m_rtMeshInstanceIdxToID[currInstance++] = currTreeLevel.m_IDs[i];
                }
            }
        }
    }

    Assert(currInstance == numInstances, "Invalid instance count.");

    const uint32_t sizeInBytes = numInstances * sizeof(RT::MeshInstance);

    PlacedResourceList<2> list;
    list.PushBuffer(sizeInBytes, false, false);
    list.PushBuffer(sizeInBytes, false, false);
    list.End();
    m_meshInstanceResHeap = GpuMemory::GetResourceHeap(list.TotalSizeInBytes());

    m_framesMeshInstances[m_frameIdx] = GpuMemory::GetPlacedHeapBufferAndInit(
        GlobalResource::RT_FRAME_MESH_INSTANCES_CURR,
        sizeInBytes,
        m_meshInstanceResHeap.Heap(),
        list.AllocInfos()[0].Offset,
        false,
        MemoryRegion{ .Data = m_frameInstanceData.data(), .SizeInBytes = sizeInBytes });
    m_framesMeshInstances[1 - m_frameIdx] = GpuMemory::GetPlacedHeapBufferAndInit(
        GlobalResource::RT_FRAME_MESH_INSTANCES_PREV,
        sizeInBytes,
        m_meshInstanceResHeap.Heap(),
        list.AllocInfos()[1].Offset,
        false,
        MemoryRegion{ .Data = m_frameInstanceData.data(), .SizeInBytes = sizeInBytes });

    // Register the shared resources
    auto& r = App::GetRenderer().GetSharedShaderResources();
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES_CURR,
        m_framesMeshInstances[m_frameIdx]);
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES_PREV,
        m_framesMeshInstances[1 - m_frameIdx]);
}

void TLAS::UpdateFrameMeshInstances_StaticToDynamic()
{
    SceneCore& scene = App::GetScene();
    const uint32_t numInstances = scene.m_numStaticInstances + scene.m_numDynamicInstances;
    const bool sceneHasEmissives = scene.NumEmissiveInstances() > 0;
    uint32_t copyStartOffset = UINT32_MAX;

    // Note: GetInstanceRtASInfo() calls must happen before m_rtASInfo is updated 
    // (by RebuildTLASInstances() & StaticBLAS::Rebuild())

    // Sort in descending order (see visualization below)
    std::sort(scene.m_pendingRtMeshModeSwitch.begin(), scene.m_pendingRtMeshModeSwitch.end(),
        [&scene](const uint64_t& lhs, const uint64_t& rhs)
        {
            RT_AS_Info lhsAsInfo = scene.GetInstanceRtASInfo(lhs);
            RT_AS_Info rhsAsInfo = scene.GetInstanceRtASInfo(rhs);
            Assert(lhsAsInfo.InstanceID == 0 && rhsAsInfo.InstanceID == 0, 
                "InstanceID for static meshes must be zero.");

            return lhsAsInfo.GeometryIndex > rhsAsInfo.GeometryIndex;
        });

    // Shift left
    // 
    // MeshesToSwitch                *       *
    // FrameInstanceArray: | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8
    //                               *
    //                     | 0 | 1 | 2 | 3 | 5 | 6 | 7 | 8 | 8
    //                     | 0 | 1 | 3 | 5 | 6 | 7 | 8 | 8 | 8
    for (size_t i = 0; i < scene.m_pendingRtMeshModeSwitch.size(); i++)
    {
        const auto instance = scene.m_pendingRtMeshModeSwitch[i];
        const RT_AS_Info asInfo = scene.GetInstanceRtASInfo(instance);
        // One less instance to move per iteration
        const int64 numToMove = numInstances - asInfo.GeometryIndex - 1 - i;

        memmove(m_frameInstanceData.data() + asInfo.GeometryIndex,
            m_frameInstanceData.data() + asInfo.GeometryIndex + 1,
            numToMove * sizeof(RT::MeshInstance));
        memmove(scene.m_rtMeshInstanceIdxToID.data() + asInfo.GeometryIndex,
            scene.m_rtMeshInstanceIdxToID.data() + asInfo.GeometryIndex + 1,
            numToMove * sizeof(uint64_t));

        // Last element is the smallest
        if (i == scene.m_pendingRtMeshModeSwitch.size() - 1)
            copyStartOffset = asInfo.GeometryIndex;
    }

    uint32_t currInstance = numInstances - (uint32)scene.m_pendingRtMeshModeSwitch.size();

    struct TreePosAndIdx
    {
        uint32_t TreeLevel;
        uint32_t LevelIdx;
        uint32_t PreSortIdx;
    };

    SmallVector<TreePosAndIdx, App::FrameAllocator> dynamicInstanceTreePositions;
    dynamicInstanceTreePositions.reserve(scene.m_numDynamicInstances);

    Assert(m_dynamicBLASes.size() + scene.m_pendingRtMeshModeSwitch.size() == 
        scene.m_numDynamicInstances, "Unexpected value.");

    // Append existing dynamic meshes
    for (size_t i = 0; i < m_dynamicBLASes.size(); i++)
    {
        dynamicInstanceTreePositions.push_back(TreePosAndIdx{ 
            .TreeLevel = m_dynamicBLASes[i].TreeLevel,
            .LevelIdx = m_dynamicBLASes[i].LevelIdx, 
            .PreSortIdx = (uint32)i });
    }

    // Append the newly converted dynamic meshes
    for (size_t i = 0; i < scene.m_pendingRtMeshModeSwitch.size(); i++)
    {
        const auto instance = scene.m_pendingRtMeshModeSwitch[i];
        const auto treePos = scene.FindTreePosFromID(instance).value();
        auto& currTreeLevel = scene.m_sceneGraph[treePos.Level];
        const auto rtFlags = RT_Flags::Decode(currTreeLevel.m_rtFlags[treePos.Offset]);

        const uint64_t meshID = currTreeLevel.m_meshIDs[treePos.Offset];
        Assert(meshID != Scene::INVALID_MESH, "Invalid mesh");

        const uint32_t emissiveTriOffset = sceneHasEmissives &&
            (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE) ?
            scene.m_emissives.FindInstance(instance).value()->BaseTriOffset :
            UINT32_MAX;

        // Unsorted, sort happens below
        FillMeshInstanceData(instance, meshID, currTreeLevel.m_toWorlds[treePos.Offset],
            emissiveTriOffset, false, currInstance);
        scene.m_rtMeshInstanceIdxToID[currInstance++] = instance;

        dynamicInstanceTreePositions.push_back(TreePosAndIdx{
            .TreeLevel = treePos.Level,
            .LevelIdx = treePos.Offset,
            .PreSortIdx = (uint32)(i + m_dynamicBLASes.size()) });
    }

    // Sort the dynamic instances based on tree position
    std::sort(dynamicInstanceTreePositions.begin(), dynamicInstanceTreePositions.end(),
        [](const TreePosAndIdx& lhs, const TreePosAndIdx& rhs)
        {
            if (lhs.TreeLevel < rhs.TreeLevel)
                return true;
            else if (lhs.TreeLevel > rhs.TreeLevel)
                return false;

            return lhs.LevelIdx < rhs.LevelIdx;
        });    
    
    // m_frameInstanceData & m_rtMeshInstanceIdxToID for new elements were filled above
    SmallVector<RT::MeshInstance, App::FrameAllocator> tempMeshInstances;
    tempMeshInstances.resize(scene.m_numDynamicInstances);
    memcpy(tempMeshInstances.data(), m_frameInstanceData.data() + scene.m_numStaticInstances,
        scene.m_numDynamicInstances * sizeof(RT::MeshInstance));

    SmallVector<uint64_t, App::FrameAllocator> tempRtMeshInstanceIdxToID;
    tempRtMeshInstanceIdxToID.resize(scene.m_numDynamicInstances);
    memcpy(tempRtMeshInstanceIdxToID.data(), scene.m_rtMeshInstanceIdxToID.data() + scene.m_numStaticInstances,
        scene.m_numDynamicInstances * sizeof(uint64_t));

    for (size_t i = 0; i < scene.m_numDynamicInstances; i++)
    {
        const auto& sorted = dynamicInstanceTreePositions[i];

        m_frameInstanceData[scene.m_numStaticInstances + i] = 
            tempMeshInstances[sorted.PreSortIdx];
        scene.m_rtMeshInstanceIdxToID[scene.m_numStaticInstances + i] = 
            tempRtMeshInstanceIdxToID[sorted.PreSortIdx];
    }

    // Copy from the smallest modified entry
    Assert(copyStartOffset != UINT32_MAX, "copyStartOffset hasn't been set.");
    const uint32_t copySizeInBytes = (numInstances - copyStartOffset) * sizeof(RT::MeshInstance);

    GpuMemory::UploadToDefaultHeapBuffer(m_framesMeshInstances[m_frameIdx], copySizeInBytes,
        MemoryRegion{ .Data = m_frameInstanceData.data() + copyStartOffset,
        .SizeInBytes = copySizeInBytes },
        copyStartOffset * sizeof(RT::MeshInstance));
}

void TLAS::UpdateFrameMeshInstances_NewTransform()
{
    SceneCore& scene = App::GetScene();
    const bool sceneHasEmissives = scene.NumEmissiveInstances() > 0;
    int64 minIdx = m_dynamicBLASes.size() - 1;
    int64 maxIdx = 0;

    for (auto it = scene.m_instanceUpdates.begin_it(); it != scene.m_instanceUpdates.end_it();
        it = scene.m_instanceUpdates.next_it(it))
    {
        const auto instance = it->Key;
        const auto treePos = scene.FindTreePosFromID(instance).value();
        const auto& treeLevel = scene.m_sceneGraph[treePos.Level];

        const auto rtFlags = RT_Flags::Decode(treeLevel.m_rtFlags[treePos.Offset]);
        const uint32_t emissiveTriOffset = sceneHasEmissives &&
            (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE) ?
            scene.m_emissives.FindInstance(instance).value()->BaseTriOffset :
            UINT32_MAX;

        auto vecIt = std::lower_bound(m_dynamicBLASes.begin(), m_dynamicBLASes.end(), treePos,
            [](const DynamicBLAS& lhs, const SceneCore::TreePos& key)
            {
                if (lhs.TreeLevel < key.Level)
                    return true;
                else if (lhs.TreeLevel > key.Level)
                    return false;

                return lhs.LevelIdx < key.Offset;
            });

        Assert(vecIt != m_dynamicBLASes.end(), "Dynamic BLAS for instance was not found.");
        const auto& blas = *vecIt;
        Assert(blas.TreeLevel == treePos.Level && blas.LevelIdx == treePos.Offset,
            "Dynamic BLAS for instance was not found.");
        const auto idx = vecIt - m_dynamicBLASes.begin();

        FillMeshInstanceData(instance, treeLevel.m_meshIDs[treePos.Offset],
            treeLevel.m_toWorlds[treePos.Offset], 
            emissiveTriOffset, 
            false, 
            blas.InstanceID);

        minIdx = Min(minIdx, idx);
        maxIdx = Max(maxIdx, idx);
    }

    Assert(minIdx <= maxIdx, "Invalid range.");
    const size_t numInstancesToCopy = maxIdx - minIdx + 1;
    const size_t sizeInBytes = numInstancesToCopy * sizeof(RT::MeshInstance);
    const size_t offset = scene.m_numStaticInstances + minIdx;

    GpuMemory::UploadToDefaultHeapBuffer(m_framesMeshInstances[m_frameIdx], (uint32)sizeInBytes,
        MemoryRegion{ .Data = m_frameInstanceData.data() + offset,
        .SizeInBytes = sizeInBytes },
        (uint32)(offset * sizeof(RT::MeshInstance)));
}

void TLAS::Update()
{
    SceneCore& scene = App::GetScene();
    m_frameIdx = 1 - m_frameIdx;
    ID3D12Heap* heap = nullptr;
    uint32_t meshTransformHeapOffsetInBytes = 0;

    // Avoid rebuild while compaction is in progress (it'll be queued up for later)
    if (!scene.m_pendingRtMeshModeSwitch.empty() && m_staticBLASCompacted)
    {
        Assert(m_staticBLAS.m_prebuildInfo.ResultDataMaxSizeInBytes != UINT32_MAX,
            "Unexpected value.");

        // Use the same buffer for scratch and compaction info
        const uint32_t compactionInfoStartOffset = (uint32_t)AlignUp(
            m_staticBLAS.m_prebuildInfo.ScratchDataSizeInBytes,
            alignof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));
        const uint32_t scratchBufferSizeInBytes = compactionInfoStartOffset +
            sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);

        PlacedResourceList<3> list;
        // BLAS (before compaction)
        list.PushBuffer((uint32_t)m_staticBLAS.m_prebuildInfo.ResultDataMaxSizeInBytes, true, true);
        // Scratch buffer & compaction info
        list.PushBuffer(scratchBufferSizeInBytes, true, false);
        // Per mesh transform during BLAS build
        list.PushBuffer(scene.m_numStaticInstances * sizeof(BLASTransform), true, false);
        list.End();

        // Do heap allocation on a background thread to avoid a hitch
        if (m_staticBLAS.m_heapAllocated.load(std::memory_order_acquire))
        {
            Assert(m_staticBLAS.m_resHeap.IsInitialized(), "Unexpected condition.");
            heap = m_staticBLAS.m_resHeap.Heap();

            auto allocs = list.AllocInfos();
            m_staticBLAS.m_BLASHeapOffsetInBytes = (uint32)allocs[0].Offset;
            m_staticBLAS.m_scratchHeapOffsetInBytes = (uint32)allocs[1].Offset;
            meshTransformHeapOffsetInBytes = (uint32)allocs[2].Offset;

            m_updateType = UPDATE_TYPE::STATIC_TO_DYNAMIC;
            m_staticBLASCompacted = false;
            m_staticBLAS.m_heapAllocationInProgress = false;
        }
        else if(!m_staticBLAS.m_heapAllocationInProgress)
        {
            const uint64_t heapSizeInBytes = list.TotalSizeInBytes();
            m_staticBLAS.m_heapAllocationInProgress = true;

            Task t("AllocateHeap", TASK_PRIORITY::BACKGROUND, [this, heapSizeInBytes]()
                {
                    m_staticBLAS.m_resHeap = GpuMemory::GetResourceHeap(heapSizeInBytes);
                    m_staticBLAS.m_heapAllocated.store(true, std::memory_order_release);
                });

            App::SubmitBackground(ZetaMove(t));
        }
    }
    // Make sure updates are performed even if compaction is in progress 
    // (when m_staticBLASCompacted = false)
    else if (!scene.m_instanceUpdates.empty())
    {
        m_updateType = UPDATE_TYPE::INSTANCE_TRANSFORM;
    }

    const bool firstTime = m_frameInstanceData.empty();

    if (firstTime || m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC)
    {
        Assert(firstTime || heap, "Invalid condition.");
        m_staticBLAS.FillMeshTransformBufferForBuild(heap, meshTransformHeapOffsetInBytes);
    }

    if(firstTime)
        RebuildFrameMeshInstanceData();
    else if (m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC)
        UpdateFrameMeshInstances_StaticToDynamic();
    else if (m_updateType == UPDATE_TYPE::INSTANCE_TRANSFORM)
        UpdateFrameMeshInstances_NewTransform();
}

void TLAS::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast.");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& gpuTimer = App::GetRenderer().GetGpuTimer();
    computeCmdList.PIXBeginEvent("RtAS");
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "RtAS");

    RebuildOrUpdateBLASes(computeCmdList);
    UpdateTLASInstances(computeCmdList);
    RebuildTLAS(computeCmdList);

    gpuTimer.EndQuery(computeCmdList, queryIdx);
    computeCmdList.PIXEndEvent();
}

void TLAS::BuildDynamicBLASes(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    SmallVector<DynamicBlasBuild, App::FrameAllocator> blasBuilds;
    blasBuilds.reserve(scene.m_numDynamicInstances);
    m_dynamicBLASes.reserve(scene.m_numDynamicInstances);

    // Skip the first level
    for (size_t treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
        auto& rtFlagVec = currTreeLevel.m_rtFlags;

        for (size_t i = 0; i < rtFlagVec.size(); i++)
        {
            Scene::RT_Flags flags = RT_Flags::Decode(rtFlagVec[i]);
            Assert((flags.RebuildFlag & flags.UpdateFlag) == 0,
                "Rebuild & update flags can't be set at the same time.");

            if (flags.MeshMode != RT_MESH_MODE::STATIC)
            {
                const TriangleMesh* mesh = scene.GetMesh(currTreeLevel.m_meshIDs[i]).value();
                const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
                const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();

                DynamicBlasBuild buildItem;
                buildItem.GeoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                buildItem.GeoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                buildItem.GeoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
                buildItem.GeoDesc.Triangles.IndexCount = mesh->m_numIndices;
                buildItem.GeoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                buildItem.GeoDesc.Triangles.Transform3x4 = 0;
                buildItem.GeoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa +
                    mesh->m_vtxBuffStartOffset * sizeof(Vertex);
                buildItem.GeoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
                buildItem.GeoDesc.Triangles.VertexCount = mesh->m_numVertices;
                buildItem.GeoDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

                blasBuilds.push_back(buildItem);

                rtFlagVec[i] = RT_Flags::Encode(flags.MeshMode, flags.InstanceMask,
                    0, 0, flags.IsOpaque);

                currTreeLevel.m_rtASInfo[i] = RT_AS_Info{
                    .GeometryIndex = 0,
                    .InstanceID = scene.m_numStaticInstances + (uint32)blasBuilds.size() - 1 };
            }
        }
    }

    auto* device = App::GetRenderer().GetDevice();
    uint32_t currBuildSizeInBytes = 0;
    uint32_t currScratchSize = 0;

    for (auto& b : blasBuilds)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
        buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        buildDesc.Inputs.Flags = BuildFlags(RT_MESH_MODE::DYNAMIC_NO_REBUILD);
        buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        buildDesc.Inputs.NumDescs = 1;
        buildDesc.Inputs.pGeometryDescs = &b.GeoDesc;

        device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &b.BuildInfo);

        Assert(b.BuildInfo.ResultDataMaxSizeInBytes > 0,
            "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

        currBuildSizeInBytes = AlignUp(currBuildSizeInBytes,
            uint32_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
        b.BlasBufferOffset = currBuildSizeInBytes;
        currBuildSizeInBytes += (uint32_t)b.BuildInfo.ResultDataMaxSizeInBytes;

        currScratchSize = AlignUp(currScratchSize,
            uint32_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
        b.ScratchBufferOffset = currScratchSize;
        currScratchSize += (uint32_t)b.BuildInfo.ScratchDataSizeInBytes;

        m_dynamicBLASes.push_back(DynamicBLAS{ .PageIdx = 0, .PageOffset = b.BlasBufferOffset });
    }

    Assert(m_dynamicBLASArenas.empty(), "bug");

    const uint32_t sizeInBytes = Max(currBuildSizeInBytes, BLAS_ARENA_PAGE_SIZE);
    auto page = GpuMemory::GetDefaultHeapBuffer("BLASArenaPage",
        sizeInBytes,
        true,
        true);

    const uint32_t offsetInBytes = AlignUp(sizeInBytes,
        uint32_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
    m_dynamicBLASArenas.push_back(ArenaPage{ .Page = ZetaMove(page), .CurrOffset = offsetInBytes });

    const uint32_t alignedScratchSize = AlignUp(currScratchSize,
        (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    if (!m_scratchBuffer.IsInitialized() ||
        m_scratchBuffer.Desc().Width < alignedScratchSize)
    {
        m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS_scratch",
            alignedScratchSize,
            D3D12_RESOURCE_STATE_COMMON,
            true);
    }

    for (auto& b : blasBuilds)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
        buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        buildDesc.Inputs.Flags = BuildFlags(RT_MESH_MODE::DYNAMIC_NO_REBUILD);
        buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        buildDesc.Inputs.NumDescs = 1;
        buildDesc.Inputs.pGeometryDescs = &b.GeoDesc;

        buildDesc.DestAccelerationStructureData = m_dynamicBLASArenas[0].Page.GpuVA() +
            b.BlasBufferOffset;
        buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA() +
            b.ScratchBufferOffset;
        buildDesc.SourceAccelerationStructureData = 0;

        cmdList.PIXBeginEvent("DynamicBLASBuild");
        cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
        cmdList.PIXEndEvent();
    }
}

void TLAS::RebuildOrUpdateBLASes(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    // From Ray Tracing Gems 1 Chapter 19:
    // "One important optimization is to ensure that any resource transition barriers that
    // are needed after BLAS updates are deferred to be executed right before the
    // TLAS build, instead of executing these right after each BLAS update. Deferral is
    // important because each of these transition barriers is a synchronization step on
    // the GPU. Having the transitions coalesced into a single point in the command buffer
    // avoids redundant synchronization that would otherwise cause the GPU to frequently
    // become idle."
    SmallVector<D3D12_BUFFER_BARRIER, App::FrameAllocator> uavBarriers;
    const bool staticBLASReady = !scene.m_numStaticInstances || m_staticBLASCompacted;

    // Compacting static BLAS requires two CPU-GPU synchronizations that'll likely
    // span multiple frames and has the following steps:
    // 
    // 1. Build static BLAS for the first time and ask the GPU for compaction info
    // 2. Wait for GPU to finish step 1
    // 3. Read back compaction info on CPU and allocate a new buffer with the 
    //    compacted size. Then, record a command for compaction operation (on GPU).
    // 4. Wait for GPU to finish step 3
    // 5. Replace BLAS from step 1 with the new compacted BLAS 
    if (!staticBLASReady || m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC)
    {
        // Step 1
        if (!m_staticBLAS.m_buffer.IsInitialized() || m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC)
        {
            m_staticBLAS.Rebuild(cmdList);

            const D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_staticBLAS.m_buffer.Resource(),
                D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
                D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

            uavBarriers.push_back(barrier);

            // Step 2
            Task t("WaitForRtAsBuild", TASK_PRIORITY::BACKGROUND, [this]()
                {
                    m_waitObj.Reset();
                    App::GetScene().GetRenderGraph()->SetFrameSubmissionWaitObj(m_waitObj);
                    m_waitObj.Wait();

                    const uint64_t fence = App::GetScene().GetRenderGraph()->GetFrameCompletionFence();
                    Assert(fence != UINT64_MAX, "Invalid fence value.");

                    App::GetRenderer().WaitForDirectQueueFenceCPU(fence);
                    m_compactionInfoReady.store(true, std::memory_order_release);
                });

            App::SubmitBackground(ZetaMove(t));
        }
        // Step 3
        else if (m_compactionInfoReady.load(std::memory_order_acquire))
        {
            // Read compaction info and submit a compaction command
            m_staticBLAS.DoCompaction(cmdList);

            m_waitObj.Reset();
            App::GetScene().GetRenderGraph()->SetFrameSubmissionWaitObj(m_waitObj);

            // Step 4
            Task t("ReleaseRtAsBuffers", TASK_PRIORITY::BACKGROUND, [this]()
                {
                    m_waitObj.Wait();

                    const uint64_t fence = App::GetScene().GetRenderGraph()->GetFrameCompletionFence();
                    Assert(fence != UINT64_MAX, "Invalid fence value.");

                    App::GetRenderer().WaitForDirectQueueFenceCPU(fence);
                    m_staticBLAS.CompactionCompletedCallback();
                });

            App::SubmitBackground(ZetaMove(t));
            m_compactionInfoReady.store(false, std::memory_order_relaxed);
        }
        // Step 5
        else if (m_staticBLAS.m_compactionCompleted.load(std::memory_order_acquire))
        {
            m_staticBLAS.m_buffer = ZetaMove(m_staticBLAS.m_bufferCompacted);
            m_staticBLASCompacted = true;
            m_updateType = UPDATE_TYPE::STATIC_BLAS_COMPACTED;

            m_staticBLAS.m_compactionCompleted.store(false, std::memory_order_relaxed);
        }
    }

    // Once in the first frame
    if (m_rebuildDynamicBLASes && scene.m_numDynamicInstances)
    {
        BuildDynamicBLASes(cmdList);

        // One barrier covers all of the builds
        D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_dynamicBLASArenas[0].Page.Resource(),
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
            D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

        uavBarriers.push_back(barrier);
    }

    if (m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC && scene.m_numDynamicInstances)
    {
        Assert(!scene.m_pendingRtMeshModeSwitch.empty(), "Unexpected condition.");

        SmallVector<DynamicBlasBuild, SystemAllocator, 3> builds;
        builds.reserve(scene.m_pendingRtMeshModeSwitch.size());

        auto* device = App::GetRenderer().GetDevice();

        for (auto instance : scene.m_pendingRtMeshModeSwitch)
        {
            const auto treePos = scene.FindTreePosFromID(instance).value();
            const auto meshID = scene.m_sceneGraph[treePos.Level].m_meshIDs[treePos.Offset];

            const TriangleMesh* mesh = scene.GetMesh(meshID).value();
            const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
            const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();

            DynamicBlasBuild build;
            build.GeoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            build.GeoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            build.GeoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
            build.GeoDesc.Triangles.IndexCount = mesh->m_numIndices;
            build.GeoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            build.GeoDesc.Triangles.Transform3x4 = 0;
            build.GeoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa +
                mesh->m_vtxBuffStartOffset * sizeof(Vertex);
            build.GeoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
            build.GeoDesc.Triangles.VertexCount = mesh->m_numVertices;
            build.GeoDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

            build.TreeLevel = treePos.Level;
            build.LevelIdx = treePos.Offset;

            builds.push_back(build);
        }

        uint32_t totalScratchDataSizeInBytes = 0;

        for (auto& b : builds)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            buildDesc.Inputs.Flags = BuildFlags(RT_MESH_MODE::DYNAMIC_NO_REBUILD);
            buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            buildDesc.Inputs.NumDescs = 1;
            buildDesc.Inputs.pGeometryDescs = &b.GeoDesc;

            device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &b.BuildInfo);

            Assert(b.BuildInfo.ResultDataMaxSizeInBytes > 0,
                "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

            totalScratchDataSizeInBytes = AlignUp(totalScratchDataSizeInBytes,
                (uint32)D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
            b.ScratchBufferOffset = totalScratchDataSizeInBytes;

            totalScratchDataSizeInBytes += (uint32)b.BuildInfo.ScratchDataSizeInBytes;
        }

        const uint32_t alignedScratchSize = AlignUp(totalScratchDataSizeInBytes,
            (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

        if (!m_scratchBuffer.IsInitialized() ||
            m_scratchBuffer.Desc().Width < totalScratchDataSizeInBytes)
        {
            m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS_scratch",
                alignedScratchSize,
                D3D12_RESOURCE_STATE_COMMON,
                true);
        }

        SmallVector<int, App::FrameAllocator, 3> touchedPages;

        for (auto& b : builds)
        {
            if (!m_dynamicBLASArenas.empty())
            {
                m_dynamicBLASArenas.back().CurrOffset = AlignUp(m_dynamicBLASArenas.back().CurrOffset,
                    (uint32_t)D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
            }

            if (m_dynamicBLASArenas.empty() ||
                (m_dynamicBLASArenas.back().CurrOffset + b.BuildInfo.ResultDataMaxSizeInBytes >=
                    m_dynamicBLASArenas.back().Page.Desc().Width))
            {
                const uint32_t sizeInBytes = Max((uint32)b.BuildInfo.ResultDataMaxSizeInBytes,
                    BLAS_ARENA_PAGE_SIZE);
                auto page = GpuMemory::GetDefaultHeapBuffer("BLASArenaPage",
                    sizeInBytes,
                    true,
                    true);

                m_dynamicBLASArenas.push_back(ArenaPage{ .Page = ZetaMove(page),
                    .CurrOffset = 0 });

                LOG_UI_INFO("Allocated dynamic BLAS page (%llu MB)...", sizeInBytes / (1024 * 1024));
            }

            // InstanceID is filled in by RebuildTLASInstances()
            m_dynamicBLASes.push_back(DynamicBLAS{ .PageIdx = (int)m_dynamicBLASArenas.size() - 1,
                .PageOffset = m_dynamicBLASArenas.back().CurrOffset,
                .TreeLevel = b.TreeLevel, 
                .LevelIdx = b.LevelIdx});
            m_dynamicBLASArenas.back().CurrOffset += (uint32)b.BuildInfo.ResultDataMaxSizeInBytes;

            touchedPages.push_back((int)(m_dynamicBLASArenas.size() - 1));

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            buildDesc.Inputs.Flags = BuildFlags(RT_MESH_MODE::DYNAMIC_NO_REBUILD);
            buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            buildDesc.Inputs.NumDescs = 1;
            buildDesc.Inputs.pGeometryDescs = &b.GeoDesc;
            buildDesc.DestAccelerationStructureData = m_dynamicBLASArenas.back().Page.GpuVA() +
                m_dynamicBLASes.back().PageOffset;
            buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA() +
                b.ScratchBufferOffset;
            buildDesc.SourceAccelerationStructureData = 0;

            cmdList.PIXBeginEvent("DynamicBLASBuild");
            cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
            cmdList.PIXEndEvent();

            std::sort(m_dynamicBLASes.begin(), m_dynamicBLASes.end(),
                [](const DynamicBLAS& lhs, const DynamicBLAS& rhs)
                {
                    if (lhs.TreeLevel < rhs.TreeLevel)
                        return true;
                    else if (lhs.TreeLevel > rhs.TreeLevel)
                        return false;

                    return lhs.LevelIdx < rhs.LevelIdx;
                });
        }

        // Insert a barrier for every used page
        if(touchedPages.size() > 1)
            std::sort(touchedPages.begin(), touchedPages.end());

        for (int i = 0; i < (int)touchedPages.size(); i++)
        {
            if (i == 0 || touchedPages[i] != touchedPages[i - 1])
            {
                D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(
                    m_dynamicBLASArenas[touchedPages[i]].Page.Resource(),
                    D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
                    D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
                    D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

                uavBarriers.push_back(barrier);
            }
        }
    }

    if (!uavBarriers.empty())
        cmdList.ResourceBarrier(uavBarriers.data(), (uint32_t)uavBarriers.size());

    m_rebuildDynamicBLASes = false;
}

void TLAS::UpdateTLASInstances(ComputeCmdList& cmdList)
{
    // When static BLAS is compacted, the GPU buffer changes and static BLAS instance
    // below also changes, which requires TLAS instance buffer to be updated.
    if (m_updateType == UPDATE_TYPE::NONE && m_tlasInstanceBuffer.IsInitialized())
        return;

    SceneCore& scene = App::GetScene();
    const uint32_t numInstances = scene.m_numDynamicInstances + (scene.m_numStaticInstances > 0);
    if (numInstances == 0)
        return;

    // Following order is important, STATIC_TO_DYNAMIC should supercede INSTANCE_TRANSFORM
    if (m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC || !m_tlasInstanceBuffer.IsInitialized())
    {
        RebuildTLASInstances(cmdList);
        scene.m_pendingRtMeshModeSwitch.clear();
    }
    else if (m_updateType == UPDATE_TYPE::STATIC_BLAS_COMPACTED)
        UpdateTLASInstances_StaticCompacted(cmdList);
    else if (m_updateType == UPDATE_TYPE::INSTANCE_TRANSFORM)
        UpdateTLASInstances_NewTransform(cmdList);
    else
        Assert(false, "Unreachable case.");

    // Wait for copy to be finished before doing compute work
    auto barrier = Direct3DUtil::BufferBarrier(m_tlasInstanceBuffer.Resource(),
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_COPY_DEST,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

    cmdList.ResourceBarrier(barrier);
}

void TLAS::RebuildTLASInstances(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();
    const uint32_t numStaticInstances = scene.m_numStaticInstances;
    const uint32_t numInstances = scene.m_numDynamicInstances + (scene.m_numStaticInstances > 0);

    m_tlasInstances.resize(numInstances);

    if (numStaticInstances)
    {
        m_tlasInstances[0] = D3D12_RAYTRACING_INSTANCE_DESC{
                .InstanceID = 0,
                .InstanceMask = RT_AS_SUBGROUP::ALL,
                .InstanceContributionToHitGroupIndex = 0,
                .Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE,
                .AccelerationStructure = m_staticBLAS.m_buffer.GpuVA()
            };

        // Identity transform for static BLAS instance
        memset(&m_tlasInstances[0].Transform, 0, sizeof(BLASTransform));
        m_tlasInstances[0].Transform[0][0] = 1.0f;
        m_tlasInstances[0].Transform[1][1] = 1.0f;
        m_tlasInstances[0].Transform[2][2] = 1.0f;
    }

    // Following traversal order must match the one in RebuildOrUpdateBLASes()
    uint32_t currDynamicInstance = 0;

    // Skip the first level
    if (scene.m_numDynamicInstances)
    {
        for (size_t treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
        {
            auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
            const auto& rtFlagVec = currTreeLevel.m_rtFlags;

            // Add one TLAS instance for every dynamic mesh
            for (size_t i = 0; i < rtFlagVec.size(); i++)
            {
                if (currTreeLevel.m_meshIDs[i] == Scene::INVALID_MESH)
                    continue;

                const auto flags = RT_Flags::Decode(rtFlagVec[i]);

                if (flags.MeshMode != RT_MESH_MODE::STATIC)
                {
                    auto& blas = m_dynamicBLASes[currDynamicInstance];
                    blas.InstanceID = numStaticInstances + currDynamicInstance;

                    D3D12_RAYTRACING_INSTANCE_DESC instance;
                    instance.InstanceID = blas.InstanceID;
                    instance.InstanceMask = flags.InstanceMask;
                    instance.InstanceContributionToHitGroupIndex = 0;
                    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                    instance.AccelerationStructure = 
                        m_dynamicBLASArenas[blas.PageIdx].Page.GpuVA() +
                        blas.PageOffset;

                    auto& M = currTreeLevel.m_toWorlds[i];

                    for (int j = 0; j < 4; j++)
                    {
                        instance.Transform[0][j] = M.m[j].x;
                        instance.Transform[1][j] = M.m[j].y;
                        instance.Transform[2][j] = M.m[j].z;
                    }

                    m_tlasInstances[(numStaticInstances > 0) + currDynamicInstance++] = instance;

                    // Update RT-AS info
                    currTreeLevel.m_rtASInfo[i].GeometryIndex = 0;
                    currTreeLevel.m_rtASInfo[i].InstanceID = instance.InstanceID;
                }
            }
        }
    }

    // When static BLAS is compacted but otherwise no other changes, only the 1st instance
    // needs to be updated
    Assert((numStaticInstances > 0) + currDynamicInstance == numInstances, "bug");
    const uint32_t sizeInBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances;

    const uint32_t alignedSizeInBytes = AlignUp(sizeInBytes,
        (uint32)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    if (!m_tlasInstanceBuffer.IsInitialized() || AlignUp(m_tlasInstanceBuffer.Desc().Width,
        (uint64)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) < alignedSizeInBytes)
    {
        m_tlasInstanceBuffer = GpuMemory::GetDefaultHeapBuffer("TLASInstances",
            alignedSizeInBytes, D3D12_RESOURCE_STATE_COMMON, false);
    }

    UploadHeapBuffer scratchBuff = GpuMemory::GetUploadHeapBuffer(sizeInBytes);
    scratchBuff.Copy(0, sizeInBytes, m_tlasInstances.data());

    cmdList.CopyBufferRegion(m_tlasInstanceBuffer.Resource(),
        0,
        scratchBuff.Resource(),
        scratchBuff.Offset(),
        sizeInBytes);
}

void TLAS::UpdateTLASInstances_StaticCompacted(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();
    Assert(scene.m_numStaticInstances, "Invalid call.");

    m_tlasInstances.resize(Max(m_tlasInstances.size(), 1llu));

    m_tlasInstances[0] = D3D12_RAYTRACING_INSTANCE_DESC{
            .InstanceID = 0,
            .InstanceMask = RT_AS_SUBGROUP::ALL,
            .InstanceContributionToHitGroupIndex = 0,
            .Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE,
            .AccelerationStructure = m_staticBLAS.m_buffer.GpuVA()
    };

    // Identity transform for static BLAS instance
    memset(&m_tlasInstances[0].Transform, 0, sizeof(BLASTransform));
    m_tlasInstances[0].Transform[0][0] = 1.0f;
    m_tlasInstances[0].Transform[1][1] = 1.0f;
    m_tlasInstances[0].Transform[2][2] = 1.0f;

    // When static BLAS is compacted but otherwise no other changes, only the 1st instance
    // needs to be updated
    constexpr uint32_t sizeInBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    constexpr uint32_t alignedSizeInBytes = AlignUp(sizeInBytes,
        (uint32)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    if (!m_tlasInstanceBuffer.IsInitialized() || AlignUp(m_tlasInstanceBuffer.Desc().Width,
        (uint64)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) < alignedSizeInBytes)
    {
        m_tlasInstanceBuffer = GpuMemory::GetDefaultHeapBuffer("TLASInstances",
            alignedSizeInBytes, D3D12_RESOURCE_STATE_COMMON, false);
    }

    UploadHeapBuffer scratchBuff = GpuMemory::GetUploadHeapBuffer(sizeInBytes);
    scratchBuff.Copy(0, sizeInBytes, m_tlasInstances.data());

    cmdList.CopyBufferRegion(m_tlasInstanceBuffer.Resource(),
        0,
        scratchBuff.Resource(),
        scratchBuff.Offset(),
        sizeInBytes);
}

void TLAS::UpdateTLASInstances_NewTransform(ComputeCmdList& cmdList)
{
    auto& scene = App::GetScene();
    const auto currFrame = App::GetTimer().GetTotalFrameCount();
    int64 minIdx = m_dynamicBLASes.size() - 1;
    int64 maxIdx = 0;
    bool hadUpdates = false;

    for (auto it = scene.m_instanceUpdates.begin_it(); it != scene.m_instanceUpdates.end_it();
        it = scene.m_instanceUpdates.next_it(it))
    {
        // Just need to update current frame's transformation for TLAS instances (same
        // check as SceneCore::UpdateWorldTransformations())
        if (it->Val < currFrame - 1)
            continue;

        const auto instance = it->Key;
        const SceneCore::TreePos treePos = scene.FindTreePosFromID(instance).value();

        auto vecIt = std::lower_bound(m_dynamicBLASes.begin(), m_dynamicBLASes.end(), treePos,
            [](const DynamicBLAS& lhs, const SceneCore::TreePos &key)
            {
                if (lhs.TreeLevel < key.Level)
                    return true;
                else if (lhs.TreeLevel > key.Level)
                    return false;

                return lhs.LevelIdx < key.Offset;
            });

        Assert(vecIt != m_dynamicBLASes.end(), "Dynamic BLAS for instance was not found.");
        const auto& blas = *vecIt;
        Assert(blas.TreeLevel == treePos.Level && blas.LevelIdx == treePos.Offset,
            "Dynamic BLAS for instance was not found.");
        const auto idx = vecIt - m_dynamicBLASes.begin();
        minIdx = Min(minIdx, idx);
        maxIdx = Max(maxIdx, idx);

        D3D12_RAYTRACING_INSTANCE_DESC tlasIns;
        tlasIns.InstanceID = blas.InstanceID;
        tlasIns.InstanceMask = 0xff;
        tlasIns.InstanceContributionToHitGroupIndex = 0;
        tlasIns.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        tlasIns.AccelerationStructure =
            m_dynamicBLASArenas[blas.PageIdx].Page.GpuVA() +
            blas.PageOffset;

        auto& M = scene.GetToWorld(instance);

        for (int j = 0; j < 4; j++)
        {
            tlasIns.Transform[0][j] = M.m[j].x;
            tlasIns.Transform[1][j] = M.m[j].y;
            tlasIns.Transform[2][j] = M.m[j].z;
        }

        // + 1 to skip static instance
        m_tlasInstances[idx + 1] = tlasIns;

        hadUpdates = true;
    }

    Assert(!hadUpdates || minIdx <= maxIdx, "Invalid range.");
    if (hadUpdates)
    {
        const size_t numInstancesToCopy = maxIdx - minIdx + 1;
        const size_t sizeInBytes = numInstancesToCopy * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);

        UploadHeapBuffer scratchBuff = GpuMemory::GetUploadHeapBuffer((uint32)sizeInBytes);
        // + 1 to skip static instance
        minIdx++;
        scratchBuff.Copy(0, (uint32)sizeInBytes, m_tlasInstances.data() + minIdx);

        cmdList.CopyBufferRegion(m_tlasInstanceBuffer.Resource(),
            minIdx * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
            scratchBuff.Resource(),
            scratchBuff.Offset(),
            sizeInBytes);
    }

    for (auto it = scene.m_instanceUpdates.begin_it(); it != scene.m_instanceUpdates.end_it();
        it = scene.m_instanceUpdates.next_it(it))
    {
        // TODO For some reason the following check has to be against currFrame - 2 instead
        // of currFrame - 1. Sequence of actions for world transfrom update from frame T to 
        // W_new:
        // T: Update was added at the tail end of frame
        // T + 1: Scene and RT transforms are updated to W_new, prev transform is changed to W_old
        // T + 2: Prev transform in scene and then mesh instance buffer are updated to W_new. Motion
        //        vectors become zero again.
        // T + 3: ?
        if (it->Val < currFrame - 2)
        {
            scene.m_instanceUpdates.erase(it->Key);
            //LOG_UI_INFO("Erased frame %llu", it->Val);
        }
    }
}

void TLAS::RebuildTLAS(ComputeCmdList& cmdList)
{
    SceneCore& scene = App::GetScene();

    const uint32_t numInstances = scene.m_numDynamicInstances + (scene.m_numStaticInstances > 0);
    if (numInstances == 0)
        return;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
    buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = numInstances;
    buildDesc.Inputs.InstanceDescs = m_tlasInstanceBuffer.GpuVA();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
    App::GetRenderer().GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(
        &buildDesc.Inputs, &prebuildInfo);
    Assert(prebuildInfo.ResultDataMaxSizeInBytes != 0, 
        "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

    const uint32_t alignedBufferSize = AlignUp((uint32_t)prebuildInfo.ResultDataMaxSizeInBytes,
        (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    if (!m_tlasBuffer[m_frameIdx].IsInitialized() ||
        m_tlasBuffer[m_frameIdx].Desc().Width < alignedBufferSize)
    {
        const size_t offset = AlignUp(alignedBufferSize, 
            (uint32_t)D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        const size_t totalSize = offset + alignedBufferSize;
        m_tlasResHeap = GpuMemory::GetResourceHeap(totalSize);

        m_tlasBuffer[m_frameIdx] = GpuMemory::GetPlacedHeapBuffer("TLAS_A",
            alignedBufferSize,
            m_tlasResHeap.Heap(),
            0,
            true,
            true);
        m_tlasBuffer[1 - m_frameIdx] = GpuMemory::GetPlacedHeapBuffer("TLAS_B",
            alignedBufferSize,
            m_tlasResHeap.Heap(),
            offset,
            true,
            true);

        cmdList.UAVBarrier(m_tlasBuffer[m_frameIdx].Resource());

        cmdList.CopyAccelerationStructure(m_tlasBuffer[1 - m_frameIdx].GpuVA(), 
            m_tlasBuffer[m_frameIdx].GpuVA());
    }

    auto& r = App::GetRenderer().GetSharedShaderResources();
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_SCENE_BVH_CURR, m_tlasBuffer[m_frameIdx]);
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_SCENE_BVH_PREV, m_tlasBuffer[1 - m_frameIdx]);
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES_CURR, 
        m_framesMeshInstances[m_frameIdx]);
    r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES_PREV, 
        m_framesMeshInstances[1 - m_frameIdx]);

    const uint32_t alignedScratchSize = AlignUp((uint32_t)prebuildInfo.ScratchDataSizeInBytes,
        (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    if (!m_scratchBuffer.IsInitialized() || 
        m_scratchBuffer.Desc().Width < alignedScratchSize)
    {
        m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("TLAS_scratch",
            alignedScratchSize,
            D3D12_RESOURCE_STATE_COMMON,
            true);
    }

    buildDesc.DestAccelerationStructureData = m_tlasBuffer[m_frameIdx].GpuVA();
    // Note that scratch buffer is reused for dynamic BLAS builds & TLAS with overlapping
    // addresses, but due to inserted barrier, it's safe.
    buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA();
    buildDesc.SourceAccelerationStructureData = 0;

    cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

     if (m_updateType == UPDATE_TYPE::STATIC_BLAS_COMPACTED ||
         m_updateType == UPDATE_TYPE::STATIC_TO_DYNAMIC)
     {
         cmdList.UAVBarrier(m_tlasBuffer[m_frameIdx].Resource());

         cmdList.CopyAccelerationStructure(m_tlasBuffer[1 - m_frameIdx].GpuVA(),
             m_tlasBuffer[m_frameIdx].GpuVA());
         cmdList.CopyResource(m_framesMeshInstances[1 - m_frameIdx].Resource(),
             m_framesMeshInstances[m_frameIdx].Resource());
     }

    // Even though TLAS was created with an initial stete of 
    // D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, the debug layer 
    // warns that "D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ and 
    // D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE can only be 
    // used with resources created using D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE 
    // or with a legacy InitialState of D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE".
#if 0
    // wait for build to be finished before doing any raytracing
    D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_tlasBuffer.Resource(),
        D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

    cmdList.ResourceBarrier(barrier);
#endif

    m_updateType = UPDATE_TYPE::NONE;
    m_ready = true;
}
