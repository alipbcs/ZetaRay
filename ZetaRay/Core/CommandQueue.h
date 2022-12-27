#pragma once

#include "../Core/Device.h"
#include "../Utility/SmallVector.h"
#include <shared_mutex>
#include <concurrentqueue-1.0.3/concurrentqueue.h>

namespace ZetaRay
{
	class CommandList;

	struct CommandQueue
	{
		CommandQueue(D3D12_COMMAND_LIST_TYPE type, const char* name = nullptr) noexcept;
		~CommandQueue() noexcept;

		CommandQueue(const CommandQueue&) = delete;
		CommandQueue& operator=(const CommandQueue&) = delete;

		ID3D12CommandQueue* GetCommandQueue() { return m_cmdQueue.Get(); }

		// Returns given command allocator back for future resuse, once the specified fence value
		// has passed on this command queue
		void ReleaseCommandAllocator(ID3D12CommandAllocator* cmdAlloc, uint64_t fenceValueToWaitFor) noexcept;
		
		// Returns a command list
		CommandList* GetCommandList() noexcept;

		// Releases command list back to the pool of available ones (command list can be safely reused 
		// after submission, unlike command allocator)
		void ReleaseCommandList(CommandList* context) noexcept;

		// Executes the give command context on this command queue
		uint64_t ExecuteCommandList(CommandList* context) noexcept;

		// Waits (CPU-side) for the given fence to reach the specified value on this command queue (blocking)
		void WaitForFenceCPU(uint64_t fenceValue) noexcept;
				
		// Flushes this command queue
		void WaitForIdle() noexcept;

		// Returns whether specified fence value has passed on this command queue
		bool IsFenceComplete(uint64_t fenceValue) noexcept;

	public:
		// Returns a command allocator. First tries to see whether one of the previously released ones
		// can be reused before creating a new one
		ID3D12CommandAllocator* GetCommandAllocator() noexcept;

		D3D12_COMMAND_LIST_TYPE m_type;
		ComPtr<ID3D12CommandQueue> m_cmdQueue;

		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_lastCompletedFenceVal = 0;
		uint64_t m_nextFenceValue = 1;
		HANDLE m_event;

		std::shared_mutex m_poolMtx;
		std::shared_mutex m_fenceMtx;

		struct ReleasedCmdAlloc
		{
			ID3D12CommandAllocator* CmdAlloc;
			uint64_t FenceToWaitFor;
		};

		SmallVector<ReleasedCmdAlloc> m_cmdAllocPool;

		// Source: https://github.com/cameron314/concurrentqueue
		struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
		{
			static const size_t BLOCK_SIZE = 512;
		};

		moodycamel::ConcurrentQueue<CommandList*, MyTraits> m_contextPool;
	};
}
