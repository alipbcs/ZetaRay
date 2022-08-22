#pragma once

#include "../Core/Constants.h"
#include "GpuMemory.h"
#include "../Utility/SmallVector.h"
#include <atomic>

namespace ZetaRay
{
	class ComputeCmdList;

	struct GpuTimer
	{
		struct alignas(32) Timing
		{
			void Reset() noexcept
			{
				Delta = -1;
				Name[0] = '\0';
			}

			static constexpr int MAX_NAME_LENGTH = 16;

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

		Vector<Timing>& GetFrameTimings(int* numQueries = nullptr) noexcept
		{
			int prevFrameIdx = m_currFrameIdx - 1 >= 0 ? m_currFrameIdx - 1 : RendererConstants::NUM_BACK_BUFFERS - 1;
			if (numQueries)
				*numQueries = m_numQueryHist[prevFrameIdx];

			return m_timings[prevFrameIdx];
		}

		// call before recording commands for a particular command list
		uint32_t BeginQuery(ComputeCmdList* cmdList, const char* name) noexcept;

		// call after all commands for a particular command list are recorded
		void EndQuery(ComputeCmdList* cmdList, uint32_t idx) noexcept;

		// call before rendering this frame
		void BeginFrame() noexcept;

		// call after all rendering commands for this frame have been submitted
		bool EndFrame(ComputeCmdList* cmdList) noexcept;

	private:
		static const int MAX_NUM_QUERIES = 32;

		ComPtr<ID3D12QueryHeap> m_queryHeap;
		ReadbackHeapBuffer m_readbackBuff;

		SmallVector<Timing> m_timings[RendererConstants::NUM_BACK_BUFFERS];
		std::atomic<int32_t> m_queryCount[RendererConstants::NUM_BACK_BUFFERS] = { 0 };

		UINT64 m_directQueueFreq;
		UINT64 m_computeQueueFreq;
		int m_currFrameIdx = 0;

		int m_numQueryHist[RendererConstants::NUM_BACK_BUFFERS] = { 0 };
	};
}
