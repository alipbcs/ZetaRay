#pragma once

#include "../Core/GpuMemory.h"
#include "RtCommon.h"
#include "../Scene/SceneCommon.h"
#include "../Support/Task.h"

namespace ZetaRay::Core
{
    class CommandList;
    class ComputeCmdList;
}

namespace ZetaRay::Util
{
    template<typename T>
    struct Span;
}

namespace ZetaRay::RT
{
    //--------------------------------------------------------------------------------------
    // Static BLAS
    //--------------------------------------------------------------------------------------

    struct StaticBLAS
    {
        void Rebuild(Core::ComputeCmdList& cmdList);
        void DoCompaction(Core::ComputeCmdList& cmdList);
        void CompactionCompletedCallback();
        void FillMeshTransformBufferForBuild(ID3D12Heap* heap = nullptr, 
            uint32_t heapOffsetInBytes = 0);

        Core::GpuMemory::Buffer m_buffer;
        Core::GpuMemory::Buffer m_bufferCompacted;
        Core::GpuMemory::Buffer m_scratch;
        Core::GpuMemory::ResourceHeap m_resHeap;

        Core::GpuMemory::ReadbackHeapBuffer m_postBuildInfoReadback;

        // 3x4 affine transformation matrix for each triangle mesh
        Core::GpuMemory::Buffer m_perMeshTransform;

        // Cache the results as it's expensive to compute
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO m_prebuildInfo = {};

        std::atomic_bool m_compactionCompleted = false;
        std::atomic_bool m_heapAllocated = false;
        bool m_heapAllocationInProgress = false;
        uint32_t m_BLASHeapOffsetInBytes = UINT32_MAX;
        uint32_t m_scratchHeapOffsetInBytes = UINT32_MAX;
    };

    //--------------------------------------------------------------------------------------
    // TLAS
    //--------------------------------------------------------------------------------------

    struct TLAS
    {
        void Update();
        void Render(Core::CommandList& cmdList);
        ZetaInline const Core::GpuMemory::Buffer& GetTLAS() const { return m_tlasBuffer[m_frameIdx];  };
        ZetaInline bool IsReady() const { return m_ready; };

    private:
        static constexpr uint32_t BLAS_ARENA_PAGE_SIZE = 4 * 1024 * 1024;

        struct ArenaPage
        {
            Core::GpuMemory::Buffer Page;
            uint32_t CurrOffset;
        };

        struct DynamicBLAS
        {
            int PageIdx;
            uint32_t PageOffset;
            uint32_t TreeLevel;
            uint32_t LevelIdx;
            uint32_t InstanceID;
        };

        enum class UPDATE_TYPE
        {
            NONE,
            STATIC_TO_DYNAMIC,
            STATIC_BLAS_COMPACTED,
            INSTANCE_TRANSFORM
        };

        // Frame mesh instances
        void FillMeshInstanceData(uint64_t instanceID, uint64_t meshID, const Math::float4x3& M,
            uint32_t emissiveTriOffset, bool staticMesh, uint32_t currInstance);
        void RebuildFrameMeshInstanceData();
        void UpdateFrameMeshInstances_StaticToDynamic();
        void UpdateFrameMeshInstances_NewTransform();

        // BLASes
        void BuildDynamicBLASes(Core::ComputeCmdList& cmdList);
        void RebuildOrUpdateBLASes(Core::ComputeCmdList& cmdList);

        // TLAS instances
        void UpdateTLASInstances(Core::ComputeCmdList& cmdList);
        void RebuildTLASInstances(Core::ComputeCmdList& cmdList);
        void UpdateTLASInstances_StaticCompacted(Core::ComputeCmdList& cmdList);
        void UpdateTLASInstances_NewTransform(Core::ComputeCmdList& cmdList);

        void RebuildTLAS(Core::ComputeCmdList& cmdList);

        StaticBLAS m_staticBLAS;
        Core::GpuMemory::Buffer m_framesMeshInstances[2];
        Core::GpuMemory::Buffer m_tlasBuffer[2];
        Core::GpuMemory::Buffer m_scratchBuffer;
        Core::GpuMemory::Buffer m_tlasInstanceBuffer;
        Core::GpuMemory::ResourceHeap m_tlasResHeap;
        Core::GpuMemory::ResourceHeap m_meshInstanceResHeap;

        // Dynamic BLAS
        Util::SmallVector<ArenaPage> m_dynamicBLASArenas;
        Util::SmallVector<DynamicBLAS> m_dynamicBLASes;

        Util::SmallVector<RT::MeshInstance> m_frameInstanceData;
        Util::SmallVector<D3D12_RAYTRACING_INSTANCE_DESC, Support::SystemAllocator, 1> m_tlasInstances;

        Support::WaitObject m_waitObj;
        std::atomic_bool m_compactionInfoReady = false;
        bool m_staticBLASCompacted = false;
        bool m_rebuildDynamicBLASes = true;
        UPDATE_TYPE m_updateType = UPDATE_TYPE::NONE;
        int m_frameIdx = 0;

        bool m_ready = false;
    };
}