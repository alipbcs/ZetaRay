#include "CommandQueue.h"
#include "RendererCore.h"
#include "CommandList.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;

//--------------------------------------------------------------------------------------
// CommandQueue
//--------------------------------------------------------------------------------------

CommandQueue::CommandQueue(D3D12_COMMAND_LIST_TYPE type)
    : m_type(type)
{}

CommandQueue::~CommandQueue()
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

void CommandQueue::Init()
{
    auto* device = App::GetRenderer().GetDevice();

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = m_type;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    CheckHR(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_cmdQueue.GetAddressOf())));
    CheckHR(device->CreateFence(m_lastCompletedFenceVal, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(m_fence.GetAddressOf())));

    m_event = CreateEventA(nullptr, false, false, nullptr);
    CheckWin32(m_event);

    m_cmdAllocPool.reserve(32);
}

uint64_t CommandQueue::ExecuteCommandList(CommandList* context)
{
    CheckHR(context->m_cmdList->Close());

    ID3D12GraphicsCommandList* contexts[1] = { context->m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, (ID3D12CommandList**)context->m_cmdList.GetAddressOf());

    ReleaseCommandAllocator(context->m_cmdAllocator, m_nextFenceValue);
    context->m_cmdAllocator = nullptr;
    App::GetRenderer().ReleaseCmdList(context);
    uint64_t ret;

    {
        std::unique_lock lock(m_fenceMtx);
        m_cmdQueue->Signal(m_fence.Get(), m_nextFenceValue);
        m_lastCompletedFenceVal = Math::Max(m_lastCompletedFenceVal, m_fence->GetCompletedValue());

        ret = m_nextFenceValue++;
    }

    return ret;
}

ID3D12CommandAllocator* CommandQueue::GetCommandAllocator()
{
    // Try to reuse
    {
        ReleasedCmdAlloc cmdAlloc{};
        bool found = false;

        {
            std::unique_lock lock(m_poolMtx);

            // only need to compare against the smallest fence in the pool
            if (!m_cmdAllocPool.empty() && m_cmdAllocPool[0].FenceToWaitFor <= m_lastCompletedFenceVal)
            {
                cmdAlloc = m_cmdAllocPool[0];
                m_cmdAllocPool.erase(0);

                std::make_heap(m_cmdAllocPool.begin(), m_cmdAllocPool.end(),
                    [](const ReleasedCmdAlloc& lhs, const ReleasedCmdAlloc& rhs)
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

    // Create a new one
    auto* device = App::GetRenderer().GetDevice();
    ID3D12CommandAllocator* cmdAlloc;
    CheckHR(device->CreateCommandAllocator(m_type, IID_PPV_ARGS(&cmdAlloc)));

    return cmdAlloc;
}

void CommandQueue::ReleaseCommandAllocator(ID3D12CommandAllocator* cmdAllocator, 
    uint64_t fenceValueToWaitFor)
{
    std::unique_lock lock(m_poolMtx);
    m_cmdAllocPool.push_back(ReleasedCmdAlloc{ 
        .CmdAlloc = cmdAllocator, 
        .FenceToWaitFor = fenceValueToWaitFor });
}

CommandList* CommandQueue::GetCommandList()
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

void CommandQueue::ReleaseCommandList(CommandList* context)
{
    m_contextPool.enqueue(context);
}

void CommandQueue::WaitForFenceCPU(uint64_t fenceValue)
{
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        // Reminder: SetEventOnCompletion() is thread safe
        CheckHR(m_fence->SetEventOnCompletion(fenceValue, m_event));
        WaitForSingleObject(m_event, INFINITE);

        //std::unique_lock lock(m_fenceMtx);
        //m_lastCompletedFenceVal = Math::Max(m_lastCompletedFenceVal, fenceValue);
    }
}

void CommandQueue::WaitForIdle()
{
    {
        std::unique_lock lock(m_fenceMtx);
        CheckHR(m_cmdQueue->Signal(m_fence.Get(), m_nextFenceValue++));
    }

    WaitForFenceCPU(m_nextFenceValue - 1);
}

bool CommandQueue::IsFenceComplete(uint64_t fenceValue)
{
    if (m_lastCompletedFenceVal < fenceValue)
        m_lastCompletedFenceVal = Math::Max(m_lastCompletedFenceVal, m_fence->GetCompletedValue());

    return m_lastCompletedFenceVal >= fenceValue;
}
