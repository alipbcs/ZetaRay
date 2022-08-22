#pragma once

#include "../Core/GpuMemory.h"
#include "../Utility/SmallVector.h"

namespace ZetaRay
{
	class CommandList;
	class ComputeCmdList;

	//--------------------------------------------------------------------------------------
	// BLAS
	//--------------------------------------------------------------------------------------

	struct StaticBLAS
	{
		void Rebuild(ComputeCmdList& cmdList) noexcept;
		void DoCompaction(ComputeCmdList& cmdList) noexcept;
		void CopyCompactionSize(ComputeCmdList& cmdList) noexcept;
		void CompactionCompletedCallback() noexcept;
		void FillMeshTransformBufferForBuild() noexcept;
		void Clear() noexcept;

		// TODO release scratch & transform buffers in the next frame
		DefaultHeapBuffer m_blasBuffer;
		DefaultHeapBuffer m_compactedBlasBuffer;
		DefaultHeapBuffer m_scratchBuffer;
		
		DefaultHeapBuffer m_postBuildInfo;
		ReadbackHeapBuffer m_postBuildInfoReadback;

		// each element contain a 3x4 transformation matrix
		UploadHeapBuffer m_perMeshTransformForBuild;
	};

	struct DynamicBLAS
	{
		DynamicBLAS() noexcept = default;

		DynamicBLAS(uint64_t insID, uint64_t meshID) noexcept
			: m_instanceID(insID),
			m_meshID(meshID)
		{}

		void Rebuild(ComputeCmdList& cmdList) noexcept;
		void Update(ComputeCmdList& cmdList) noexcept;
		void Clear() noexcept;

		// TODO release scratch & transform buffers in the next frame
		DefaultHeapBuffer m_blasBuffer;
		DefaultHeapBuffer m_scratchBuffer;
		
		uint64_t m_instanceID = -1;
		uint64_t m_meshID = -1;

		uint32_t m_frameBuilt = -1;
	};

	//--------------------------------------------------------------------------------------
	// TLAS
	//--------------------------------------------------------------------------------------

	struct TLAS
	{
		void Render(CommandList& cmdList) noexcept;
		void BuildFrameMeshInstanceData() noexcept;
		DefaultHeapBuffer& GetTLAS() noexcept { return m_tlasBuffer;  };
		void Clear() noexcept;

	private:
		void RebuildTLAS(ComputeCmdList& cmdList) noexcept;
		void RebuildOrUpdateBLASes(ComputeCmdList& cmdList) noexcept;
		int FindDynamicBLAS(uint64_t id) noexcept;

		StaticBLAS m_staticBLAS;
		SmallVector<DynamicBLAS> m_dynamicBLASes;

		DefaultHeapBuffer m_framesMeshInstances;

		// CmdList->BuildAS() updates in-place which means shaders from previous frame
		// might still be referencing the TLAS when RebuildTLAS is submitted
		DefaultHeapBuffer m_tlasBuffer;
		DefaultHeapBuffer m_scratchBuff;
		UploadHeapBuffer m_tlasInstanceBuff;

		uint32_t m_staticBLASrebuiltFrame = -1;
	};
}