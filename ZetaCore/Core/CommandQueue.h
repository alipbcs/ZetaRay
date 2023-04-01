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
		CommandQueue(D3D12_COMMAND_LIST_TYPE type, const char* name = nullptr) noexcept;
		~CommandQueue() noexcept;

		CommandQueue(const CommandQueue&) = delete;
		CommandQueue& operator=(const CommandQueue&) = delete;

		ZetaInline ID3D12CommandQueue* GetCommandQueue() { return m_cmdQueue.Get(); }
		CommandList* GetCommandList() noexcept;

		// Returns the command allocator for future resuse (once the specified fence value
		// has passed on this command queue)
		void ReleaseCommandAllocator(ID3D12CommandAllocator* cmdAlloc, uint64_t fenceValueToWaitFor) noexcept;
		
		// Releases command list back to the pool of available ones (command lists can be safely reused 
		// after submission, unlike command allocator)
		void ReleaseCommandList(CommandList* context) noexcept;

		// Waits (CPU side) for the given fence to reach the specified value on this command queue (blocking)
		void WaitForFenceCPU(uint64_t fenceValue) noexcept;
				
		uint64_t ExecuteCommandList(CommandList* context) noexcept;
		void WaitForIdle() noexcept;
		bool IsFenceComplete(uint64_t fenceValue) noexcept;

	public:
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

		Util::SmallVector<ReleasedCmdAlloc, App::ThreadAllocator> m_cmdAllocPool;

		struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
		{
			static const size_t BLOCK_SIZE = 512;
		};

		moodycamel::ConcurrentQueue<CommandList*, MyTraits> m_contextPool;
	};
}
