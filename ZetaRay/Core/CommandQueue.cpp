#include "CommandQueue.h"
#include "../Win32/App.h"
#include "Renderer.h"
#include "CommandList.h"

using namespace ZetaRay;
using namespace ZetaRay::Win32;
using namespace ZetaRay::Core;

//--------------------------------------------------------------------------------------
// CommandQueue
//--------------------------------------------------------------------------------------

CommandQueue::CommandQueue(D3D12_COMMAND_LIST_TYPE type, const char* name) noexcept
	: m_type(type)
{
	auto* device = App::GetRenderer().GetDevice();

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = type;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	CheckHR(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_cmdQueue.GetAddressOf())));
	
	if (name)
		SET_D3D_OBJ_NAME(m_cmdQueue, name);

	CheckHR(device->CreateFence(m_lastCompletedFenceVal, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));

	m_event = CreateEventA(nullptr, false, false, "CommandQueue");
	CheckWin32(m_event);
}

CommandQueue::~CommandQueue() noexcept
{
	WaitForIdle();

	CommandList* ctx;
	while (m_contextPool.try_dequeue(ctx))
		delete ctx;

	if (m_event)
		CloseHandle(m_event);

	for (auto& it : m_cmdAllocPool)
		it.CmdAlloc->Release();
}

uint64_t CommandQueue::ExecuteCommandList(CommandList* context) noexcept
{
	CheckHR(context->m_cmdList->Close());

	ID3D12GraphicsCommandList* contexts[1] = { context->m_cmdList.Get() };
	m_cmdQueue->ExecuteCommandLists(1, (ID3D12CommandList**)context->m_cmdList.GetAddressOf());

	ReleaseCommandAllocator(context->m_cmdAllocator, m_nextFenceValue);
	context->m_cmdAllocator = nullptr;
	App::GetRenderer().ReleaseCmdList(context);

	{
		std::unique_lock lock(m_fenceMtx);
		m_cmdQueue->Signal(m_fence.Get(), m_nextFenceValue);
		m_lastCompletedFenceVal = std::max(m_lastCompletedFenceVal, m_fence->GetCompletedValue());
		
		m_nextFenceValue++;
	}

	return m_nextFenceValue - 1;
}

ID3D12CommandAllocator* CommandQueue::GetCommandAllocator() noexcept
{
	{
		ReleasedCmdAlloc cmdAlloc;
		bool found = false;

		{
			std::unique_lock lock(m_poolMtx);

			// only need to compare against the smallest fence in the pool
			if (!m_cmdAllocPool.empty() && m_cmdAllocPool[0].FenceToWaitFor <= m_lastCompletedFenceVal)
			{
				cmdAlloc = m_cmdAllocPool[0];
				m_cmdAllocPool.erase(0);
				std::make_heap(m_cmdAllocPool.begin(), m_cmdAllocPool.end(),
					[](const ReleasedCmdAlloc& lhs, const ReleasedCmdAlloc& rhs) noexcept
					{
						return lhs.FenceToWaitFor > rhs.FenceToWaitFor;
					});

				found = true;
			}
		}

		if (found)
		{
			CheckHR(cmdAlloc.CmdAlloc->Reset());
			return cmdAlloc.CmdAlloc;
		}
	}

	// reuse wasn't possible, create a new one
	auto* device = App::GetRenderer().GetDevice();
	ID3D12CommandAllocator* cmdAlloc;
	CheckHR(device->CreateCommandAllocator(m_type, IID_PPV_ARGS(&cmdAlloc)));

	return cmdAlloc;
}

void CommandQueue::ReleaseCommandAllocator(ID3D12CommandAllocator* cmdAllocator, uint64_t fenceValueToWaitFor) noexcept
{
	std::unique_lock lock(m_poolMtx);
	m_cmdAllocPool.push_back(ReleasedCmdAlloc{ 
		.CmdAlloc = cmdAllocator, 
		.FenceToWaitFor = fenceValueToWaitFor });
}

CommandList* CommandQueue::GetCommandList() noexcept
{
	auto* cmdAlloc = GetCommandAllocator();
	CommandList* ctx;

	if (m_contextPool.try_dequeue(ctx))
	{
		ctx->Reset(cmdAlloc);
		return ctx;
	}

	CommandList* context = new(std::nothrow) CommandList(m_type, cmdAlloc);

	return context;
}

void CommandQueue::ReleaseCommandList(CommandList* context) noexcept
{
	m_contextPool.enqueue(context);
}

void CommandQueue::WaitForFenceCPU(uint64_t fenceValue) noexcept
{
	if (m_fence->GetCompletedValue() < fenceValue)
	{
		std::unique_lock lock(m_fenceMtx);
		CheckHR(m_fence->SetEventOnCompletion(fenceValue, m_event));	
		WaitForSingleObject(m_event, INFINITE);

		m_lastCompletedFenceVal = fenceValue;
	}
}

void CommandQueue::WaitForIdle() noexcept
{
	{
		std::unique_lock lock(m_fenceMtx);
		CheckHR(m_cmdQueue->Signal(m_fence.Get(), m_nextFenceValue++));
	}

	WaitForFenceCPU(m_nextFenceValue - 1);
}

bool CommandQueue::IsFenceComplete(uint64_t fenceValue) noexcept
{
	if (m_lastCompletedFenceVal < fenceValue)
		m_lastCompletedFenceVal = std::max(m_lastCompletedFenceVal, m_fence->GetCompletedValue());

	return m_lastCompletedFenceVal >= fenceValue;
}
