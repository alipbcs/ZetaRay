#pragma once

#include "../Core/GpuMemory.h"
#include "../Utility/SmallVector.h"

namespace ZetaRay::Core
{
	class CommandList;
	class ComputeCmdList;
}

namespace ZetaRay::RT
{
	//--------------------------------------------------------------------------------------
	// BLAS
	//--------------------------------------------------------------------------------------

	struct StaticBLAS
	{
		void Rebuild(Core::ComputeCmdList& cmdList);
		void DoCompaction(Core::ComputeCmdList& cmdList);
		void CopyCompactionSize(Core::ComputeCmdList& cmdList);
		void CompactionCompletedCallback();
		void FillMeshTransformBufferForBuild();
		void Clear();

		// TODO release scratch & transform buffers in the next frame
		Core::GpuMemory::DefaultHeapBuffer m_blasBuffer;
		Core::GpuMemory::DefaultHeapBuffer m_compactedBlasBuffer;
		Core::GpuMemory::DefaultHeapBuffer m_scratchBuffer;
		
		Core::GpuMemory::DefaultHeapBuffer m_postBuildInfo;
		Core::GpuMemory::ReadbackHeapBuffer m_postBuildInfoReadback;

		// each element containa a 3x4 affine transformation matrix
		Core::GpuMemory::DefaultHeapBuffer m_perMeshTransformForBuild;
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
		Core::GpuMemory::DefaultHeapBuffer& GetTLAS() { return m_tlasBuffer;  };
		void Clear();

	private:
		void RebuildTLAS(Core::ComputeCmdList& cmdList);
		void RebuildTLASInstances(Core::ComputeCmdList& cmdList);
		void RebuildOrUpdateBLASes(Core::ComputeCmdList& cmdList);
		int FindDynamicBLAS(uint64_t id);

		StaticBLAS m_staticBLAS;
		Util::SmallVector<DynamicBLAS> m_dynamicBLASes;

		Core::GpuMemory::DefaultHeapBuffer m_framesMeshInstances;

		// CmdList->BuildAS() updates in-place which means shaders from the previous frame
		// might still be referencing the TLAS when RebuildTLAS is submitted
		Core::GpuMemory::DefaultHeapBuffer m_tlasBuffer;
		Core::GpuMemory::DefaultHeapBuffer m_scratchBuff;
		Core::GpuMemory::DefaultHeapBuffer m_tlasInstanceBuff;

		uint32_t m_staticBLASrebuiltFrame = uint32_t(-1);
	};
}