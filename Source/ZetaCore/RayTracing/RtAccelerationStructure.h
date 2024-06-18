#pragma once

#include "../Core/GpuMemory.h"
#include "RtCommon.h"
#include "../Scene/SceneCommon.h"

namespace ZetaRay::Core
{
    class CommandList;
    class ComputeCmdList;
}

namespace ZetaRay::RT
{
    struct BLASTransform
    {
        float M[3][4];
    };

    struct DynamicBlasBuildItem
    {
        D3D12_RAYTRACING_GEOMETRY_DESC GeoDesc;
        uint32_t* BlasBufferOffset;
        uint32_t ScratchBufferOffset;
    };

    //--------------------------------------------------------------------------------------
    // Static BLAS
    //--------------------------------------------------------------------------------------

    struct StaticBLAS
    {
        void Rebuild(Core::ComputeCmdList& cmdList);
        void DoCompaction(Core::ComputeCmdList& cmdList);
        void CompactionCompletedCallback();
        void FillMeshTransformBufferForBuild();
        void Clear();

        Core::GpuMemory::DefaultHeapBuffer m_buffer;
        Core::GpuMemory::DefaultHeapBuffer m_bufferCompacted;
        Core::GpuMemory::DefaultHeapBuffer m_scratch;

        uint32_t m_compactionInfoStartOffset;
        Core::GpuMemory::ReadbackHeapBuffer m_postBuildInfoReadback;

        // 3x4 affine transformation matrix for each triangle mesh
        Core::GpuMemory::DefaultHeapBuffer m_perMeshTransform;
    };

    //--------------------------------------------------------------------------------------
    // Dynamic BLAS
    //--------------------------------------------------------------------------------------

    struct DynamicBLAS
    {
        DynamicBLAS() = default;
        DynamicBLAS(uint64_t insID, uint64_t meshID)
            : m_instanceID(insID),
            m_meshID(meshID)
        {}

        DynamicBlasBuildItem Rebuild();

        uint64_t m_instanceID = Scene::INVALID_INSTANCE;
        uint64_t m_meshID = Scene::INVALID_MESH;
        uint32_t m_blasBufferOffset = 0;
    };

    //--------------------------------------------------------------------------------------
    // TLAS
    //--------------------------------------------------------------------------------------

    struct TLAS
    {
        void Render(Core::CommandList& cmdList);
        void BuildFrameMeshInstanceData();
        void BuildStaticBLASTransforms();
        const Core::GpuMemory::DefaultHeapBuffer& GetTLAS() const { return m_tlasBuffer;  };
        bool IsReady() const { return m_ready; };

    private:
        void RebuildTLAS(Core::ComputeCmdList& cmdList);
        void RebuildTLASInstances(Core::ComputeCmdList& cmdList);
        void RebuildOrUpdateBLASes(Core::ComputeCmdList& cmdList);
        void RebuildDynamicBLASes(Core::ComputeCmdList& cmdList);

        StaticBLAS m_staticBLAS;
        Core::GpuMemory::DefaultHeapBuffer m_dynamicBlasBuffer;
        Util::SmallVector<DynamicBLAS> m_dynamicBLASes;

        Core::GpuMemory::DefaultHeapBuffer m_framesMeshInstances;
        Core::GpuMemory::DefaultHeapBuffer m_tlasBuffer;
        Core::GpuMemory::DefaultHeapBuffer m_scratchBuffer;
        Core::GpuMemory::DefaultHeapBuffer m_tlasInstanceBuff;

        Util::SmallVector<RT::MeshInstance> m_frameInstanceData;

        uint32_t m_staticBLASrebuiltFrame = UINT32_MAX;
        bool m_ready = false;
    };
}