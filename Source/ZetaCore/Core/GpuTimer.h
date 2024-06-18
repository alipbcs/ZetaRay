#pragma once

#include <Core/Config.h>
#include "GpuMemory.h"
#include <atomic>

namespace ZetaRay::Core
{
    class ComputeCmdList;

    struct GpuTimer
    {
        struct alignas(32) Timing
        {
            static constexpr int MAX_NAME_LENGTH = 32;

            char Name[MAX_NAME_LENGTH];
            double Delta;
            D3D12_COMMAND_LIST_TYPE ExecutionQueue;
        };

        GpuTimer() = default;
        ~GpuTimer() = default;

        GpuTimer(GpuTimer&&) = delete;
        GpuTimer& operator=(GpuTimer&&) = delete;

        void Init();
        void Shutdown();
        Util::Span<Timing> GetFrameTimings();

        // Call before recording commands for a particular command list
        uint32_t BeginQuery(ComputeCmdList& cmdList, const char* name);

        // Call after all commands for a particular command list are recorded
        void EndQuery(ComputeCmdList& cmdList, uint32_t idx);

        // Call before rendering this frame
        void BeginFrame();

        // Call after all rendering commands for this frame have been submitted
        void EndFrame(ComputeCmdList& cmdList);

    private:
        static constexpr uint32_t MAX_NUM_QUERIES = 32;

        ComPtr<ID3D12QueryHeap> m_queryHeap;
        GpuMemory::ReadbackHeapBuffer m_readbackBuff;

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
