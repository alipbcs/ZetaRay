#pragma once

#include "../Core/Constants.h"
#include "GpuMemory.h"
#include "../Utility/Span.h"

#include <atomic>

namespace ZetaRay::Core
{
	class ComputeCmdList;

	struct GpuTimer
	{
		struct alignas(32) Timing
		{
			//void Reset() noexcept
			//{
			//	Delta = -1;
			//	Name[0] = '\0';
			//}

			static constexpr int MAX_NAME_LENGTH = 20;

			char Name[MAX_NAME_LENGTH];
			double Delta;
			D3D12_COMMAND_LIST_TYPE ExecutionQueue;
		};

		GpuTimer() noexcept = default;
		~GpuTimer() noexcept = default;

		GpuTimer(GpuTimer&) = delete;
		GpuTimer& operator=(GpuTimer&) = delete;

		void Init() noexcept;
		void Shutdown() noexcept;

		Util::Span<Timing> GetFrameTimings() noexcept;

		// call before recording commands for a particular command list
		uint32_t BeginQuery(ComputeCmdList& cmdList, const char* name) noexcept;

		// call after all commands for a particular command list are recorded
		void EndQuery(ComputeCmdList& cmdList, uint32_t idx) noexcept;

		// call before rendering this frame
		void BeginFrame() noexcept;

		// call after all rendering commands for this frame have been submitted
		void EndFrame(ComputeCmdList& cmdList) noexcept;

	private:
		static const uint32_t MAX_NUM_QUERIES = 32;

		ComPtr<ID3D12QueryHeap> m_queryHeap;
		ReadbackHeapBuffer m_readbackBuff;

		Util::SmallVector<Timing> m_timings[Constants::NUM_BACK_BUFFERS + 1];
		int m_queryCounts[Constants::NUM_BACK_BUFFERS + 1] = { 0 };
		std::atomic<int32_t> m_frameQueryCount;

		UINT64 m_directQueueFreq;
		UINT64 m_computeQueueFreq;
		
		int m_currFrameIdx = 0;
		int m_nextCompletedFrameIdx = 0;
		uint64_t m_fenceVals[Constants::NUM_BACK_BUFFERS] = { 0 };
		uint64_t m_nextFenceVal = 1;
		ComPtr<ID3D12Fence> m_fence;
	};
}
