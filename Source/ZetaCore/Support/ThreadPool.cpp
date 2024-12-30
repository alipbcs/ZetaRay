#include "ThreadPool.h"
#include "../App/Log.h"

using namespace ZetaRay::Support;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// ThreadPool
//--------------------------------------------------------------------------------------

void ThreadPool::Init(int poolSize, int totalNumThreads, const wchar_t* threadNamePrefix, 
    THREAD_PRIORITY priority, int threadIdxOffset)
{
    m_threadPoolSize = poolSize;
    m_totalNumThreads = totalNumThreads;

    // Tokens below have to conisder that threads outside this thread pool
    // (e.g. the main thread) may also insert tasks and occasionally execute 
    // tasks (for example when trying to pump the queue to empty it)

    // Consumers
    {
        // +1 for main therad
        uintptr_t curr = reinterpret_cast<uintptr_t>(m_consumerTokensMem);
        for (int i = 0; i < m_totalNumThreads; i++)
        {
            new (reinterpret_cast<void*>(curr)) moodycamel::ConsumerToken(m_taskQueue);
            curr += sizeof(moodycamel::ConsumerToken);
        }

        m_consumerTokens = reinterpret_cast<moodycamel::ConsumerToken*>(m_consumerTokensMem);
    }

    // Producers
    {
        // +1 for main therad
        uintptr_t curr = reinterpret_cast<uintptr_t>(m_producerTokensMem);
        for (int i = 0; i < m_totalNumThreads; i++)
        {
            new (reinterpret_cast<void*>(curr)) moodycamel::ProducerToken(m_taskQueue);
            curr += sizeof(moodycamel::ProducerToken);
        }

        m_producerTokens = reinterpret_cast<moodycamel::ProducerToken*>(m_producerTokensMem);
    }

    for (int i = 0; i < m_threadPoolSize; i++)
    {
        // g_threadIdx needs to be unique for all threads - threadIdxOffset is used
        // to account for other threads
        m_threadPool[i] = std::thread(&ThreadPool::WorkerThread, this, threadIdxOffset + i);

        wchar_t buffer[32];
        //swprintf(buff, L"ZetaWorker_%d", i);
        swprintf(buffer, L"%ls_%d", threadNamePrefix, i);
        App::SetThreadDesc(m_threadPool[i].native_handle(), buffer);

        App::SetThreadPriority(m_threadPool[i].native_handle(), priority);
    }
}

void ThreadPool::Start()
{
    m_start.store(true, std::memory_order_release);
}

void ThreadPool::Shutdown()
{
    // Relaxed since Enqueue has a release op
    m_shutdown.store(true, std::memory_order_relaxed);

    // Upon observing shutdown flag to be true, all the threads are going to exit

    for (int i = 0; i < m_threadPoolSize; i++)
    {
        Task t("NoOp", TASK_PRIORITY::NORMAL, []() {});
        Enqueue(ZetaMove(t));
    }

    for (int i = 0; i < m_threadPoolSize; i++)
        m_threadPool[i].join();
}

void ThreadPool::Enqueue(Task&& task)
{
    bool memAllocFailed = m_taskQueue.enqueue(m_producerTokens[g_threadIdx], ZetaMove(task));
    Assert(memAllocFailed, "moodycamel::ConcurrentQueue couldn't allocate memory.");

    m_numTasksToFinishTarget.fetch_add(1, std::memory_order_relaxed);
    m_numTasksInQueue.fetch_add(1, std::memory_order_release);
}

void ThreadPool::Enqueue(TaskSet&& ts)
{
    Assert(ts.IsFinalized(), "Given TaskSet is not finalized.");

    m_numTasksToFinishTarget.fetch_add(ts.GetSize(), std::memory_order_relaxed);
    m_numTasksInQueue.fetch_add(ts.GetSize(), std::memory_order_release);
    auto tasks = ts.GetTasks();

    bool memAllocFailed = m_taskQueue.enqueue_bulk(m_producerTokens[g_threadIdx],
        std::make_move_iterator(tasks.data()), tasks.size());
    Assert(memAllocFailed, "moodycamel::ConcurrentQueue couldn't allocate memory.");
}

void ThreadPool::PumpUntilEmpty()
{
    Task task;

    // "try_dequeue()" returning false doesn't guarantee that queue is empty
    while (m_numTasksInQueue.load(std::memory_order_acquire) != 0)
    {
        if (m_taskQueue.try_dequeue(m_consumerTokens[g_threadIdx], task))
        {
            m_numTasksInQueue.fetch_sub(1, std::memory_order_relaxed);

            const int taskHandle = task.GetSignalHandle();

            // Block if this task depends on other unfinished tasks
            App::WaitForAdjacentHeadNodes(taskHandle);

            task.DoTask();

            // Signal dependent tasks that this task has finished
            auto adjacencies = task.GetAdjacencies();
            if (adjacencies.size() > 0)
                App::SignalAdjacentTailNodes(adjacencies);

            m_numTasksFinished.fetch_add(1, std::memory_order_release);
        }
    }
}

bool ThreadPool::TryFlush()
{
    const bool success = m_numTasksFinished.load(std::memory_order_acquire) == 
        m_numTasksToFinishTarget.load(std::memory_order_acquire);
    if (!success)
    {
        PumpUntilEmpty();
    }
    else
    {
        // Reset the counters
        m_numTasksFinished.store(0, std::memory_order_relaxed);
        m_numTasksToFinishTarget.store(0, std::memory_order_relaxed);
    }

    return success;
}

void ThreadPool::WorkerThread(int idx)
{
    Assert(g_threadIdx == -1, "Two or more threads have the same global index.");
    g_threadIdx = idx;

    while (!m_start.load(std::memory_order_acquire)) {}

    LOG_UI(INFO, "Thread %d waiting for tasks...\n", g_threadIdx);

    while (true)
    {
        Task task;

        // Exit
        if (m_shutdown.load(std::memory_order_acquire))
            break;

        // block if there aren't any tasks
        m_taskQueue.wait_dequeue(m_consumerTokens[g_threadIdx], task);
        m_numTasksInQueue.fetch_sub(1, std::memory_order_acquire);

        const int taskHandle = task.GetSignalHandle();

        // Block if this task has unfinished dependencies
        if(task.GetPriority() != TASK_PRIORITY::BACKGROUND)
            App::WaitForAdjacentHeadNodes(taskHandle);

        task.DoTask();

        // Signal dependent tasks that this task has finished
        if (task.GetPriority() != TASK_PRIORITY::BACKGROUND)
        {
            auto adjacencies = task.GetAdjacencies();
            if (adjacencies.size() > 0)
                App::SignalAdjacentTailNodes(adjacencies);
        }

        m_numTasksFinished.fetch_add(1, std::memory_order_release);
    }

    LOG_UI(INFO, "Thread %d exiting...\n", g_threadIdx);
}
