#pragma once

#include "../Core/Device.h"
#include "../Utility/SmallVector.h"
#include "../App/App.h"
#include <shared_mutex>
#include <concurrentqueue/concurrentqueue.h>

namespace ZetaRay::Core
{
    class CommandList;

    struct CommandQueue
    {
        explicit CommandQueue(D3D12_COMMAND_LIST_TYPE type);
        ~CommandQueue();
        CommandQueue(const CommandQueue&) = delete;
        CommandQueue& operator=(const CommandQueue&) = delete;

        void Init();
        ZetaInline ID3D12CommandQueue* GetCommandQueue() { return m_cmdQueue.Get(); }
        CommandList* GetCommandList();

        // Returns the command allocator for future reuse (once the specified fence value
        // has passed on this command queue)
        void ReleaseCommandAllocator(ID3D12CommandAllocator* cmdAlloc, uint64_t fenceValueToWaitFor);

        // Releases command list back to the pool of available ones (command lists can be safely reused 
        // after submission, unlike command allocator)
        void ReleaseCommandList(CommandList* context);

        // Waits (CPU side) for the given fence to reach the specified value on this command queue (blocking)
        void WaitForFenceCPU(uint64_t fenceValue);

        uint64_t ExecuteCommandList(CommandList* context);
        void WaitForIdle();
        bool IsFenceComplete(uint64_t fenceValue);

    public:
        ID3D12CommandAllocator* GetCommandAllocator();

        D3D12_COMMAND_LIST_TYPE m_type;
        ComPtr<ID3D12CommandQueue> m_cmdQueue;
        ComPtr<ID3D12Fence> m_fence;
        uint64_t m_lastCompletedFenceVal = 0;
        uint64_t m_nextFenceValue = 1;
        HANDLE m_event;
        std::shared_mutex m_poolMtx;
        std::shared_mutex m_fenceMtx;
        bool m_initialized = false;

        struct ReleasedCmdAlloc
        {
            ID3D12CommandAllocator* CmdAlloc;
            uint64_t FenceToWaitFor;
        };

        Util::SmallVector<ReleasedCmdAlloc, Support::SystemAllocator, 8> m_cmdAllocPool;

        struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
        {
            static const size_t BLOCK_SIZE = 512;
        };

        moodycamel::ConcurrentQueue<CommandList*, MyTraits> m_contextPool;
    };
}
