#pragma once

#include "../Core/GpuMemory.h"
#include "RtCommon.h"

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

	//--------------------------------------------------------------------------------------
	// BLAS
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

	struct DynamicBLAS
	{
		DynamicBLAS() = default;
		DynamicBLAS(uint64_t insID, uint64_t meshID)
			: m_instanceID(insID),
			m_meshID(meshID)
		{}

		void Rebuild(Core::ComputeCmdList& cmdList);
		void Update(Core::ComputeCmdList& cmdList);
		void Clear();

		// TODO release scratch & transform buffers in the next frame
		Core::GpuMemory::DefaultHeapBuffer m_blasBuffer;
		Core::GpuMemory::DefaultHeapBuffer m_scratchBuffer;
		
		uint64_t m_instanceID = uint64_t(-1);
		uint64_t m_meshID = uint64_t(-1);
		uint32_t m_frameBuilt = uint32_t(-1);
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
		void Clear();
		bool IsReady() const { return m_ready; };

	private:
		void RebuildTLAS(Core::ComputeCmdList& cmdList);
		void RebuildTLASInstances(Core::ComputeCmdList& cmdList);
		void RebuildOrUpdateBLASes(Core::ComputeCmdList& cmdList);
		int FindDynamicBLAS(uint64_t id);

		StaticBLAS m_staticBLAS;
		Util::SmallVector<DynamicBLAS> m_dynamicBLASes;

		Core::GpuMemory::DefaultHeapBuffer m_framesMeshInstances;
		Core::GpuMemory::DefaultHeapBuffer m_tlasBuffer;
		Core::GpuMemory::DefaultHeapBuffer m_scratchBuff;
		Core::GpuMemory::DefaultHeapBuffer m_tlasInstanceBuff;

		Util::SmallVector<RT::MeshInstance> m_frameInstanceData;

		uint32_t m_staticBLASrebuiltFrame = uint32_t(-1);
		bool m_ready = false;
	};
}