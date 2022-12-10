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
		void Rebuild(Core::ComputeCmdList& cmdList) noexcept;
		void DoCompaction(Core::ComputeCmdList& cmdList) noexcept;
		void CopyCompactionSize(Core::ComputeCmdList& cmdList) noexcept;
		void CompactionCompletedCallback() noexcept;
		void FillMeshTransformBufferForBuild() noexcept;
		void Clear() noexcept;

		// TODO release scratch & transform buffers in the next frame
		Core::DefaultHeapBuffer m_blasBuffer;
		Core::DefaultHeapBuffer m_compactedBlasBuffer;
		Core::DefaultHeapBuffer m_scratchBuffer;
		
		Core::DefaultHeapBuffer m_postBuildInfo;
		Core::ReadbackHeapBuffer m_postBuildInfoReadback;

		// each element contain a 3x4 transformation matrix
		Core::UploadHeapBuffer m_perMeshTransformForBuild;
	};

	struct DynamicBLAS
	{
		DynamicBLAS() noexcept = default;

		DynamicBLAS(uint64_t insID, uint64_t meshID) noexcept
			: m_instanceID(insID),
			m_meshID(meshID)
		{}

		void Rebuild(Core::ComputeCmdList& cmdList) noexcept;
		void Update(Core::ComputeCmdList& cmdList) noexcept;
		void Clear() noexcept;

		// TODO release scratch & transform buffers in the next frame
		Core::DefaultHeapBuffer m_blasBuffer;
		Core::DefaultHeapBuffer m_scratchBuffer;
		
		uint64_t m_instanceID = -1;
		uint64_t m_meshID = -1;

		uint32_t m_frameBuilt = -1;
	};

	//--------------------------------------------------------------------------------------
	// TLAS
	//--------------------------------------------------------------------------------------

	struct TLAS
	{
		void Render(Core::CommandList& cmdList) noexcept;
		void BuildFrameMeshInstanceData() noexcept;
		Core::DefaultHeapBuffer& GetTLAS() noexcept { return m_tlasBuffer;  };
		void Clear() noexcept;

	private:
		void RebuildTLAS(Core::ComputeCmdList& cmdList) noexcept;
		void RebuildOrUpdateBLASes(Core::ComputeCmdList& cmdList) noexcept;
		int FindDynamicBLAS(uint64_t id) noexcept;

		StaticBLAS m_staticBLAS;
		Util::SmallVector<DynamicBLAS, App::ThreadAllocator> m_dynamicBLASes;

		Core::DefaultHeapBuffer m_framesMeshInstances;

		// CmdList->BuildAS() updates in-place which means shaders from previous frame
		// might still be referencing the TLAS when RebuildTLAS is submitted
		Core::DefaultHeapBuffer m_tlasBuffer;
		Core::DefaultHeapBuffer m_scratchBuff;
		Core::UploadHeapBuffer m_tlasInstanceBuff;

		uint32_t m_staticBLASrebuiltFrame = -1;
	};
}