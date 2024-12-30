#pragma once

#include "Task.h"
#include "concurrentqueue/blockingconcurrentqueue.h"

namespace ZetaRay::Support
{
    class ThreadPool
    {
    public:
        ThreadPool() = default;
        ~ThreadPool() = default;
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        void Init(int poolSize, int totalNumThreads, const wchar_t* threadNamePrefix, 
            App::THREAD_PRIORITY priority, int threadIdxOffset);
        void Start();
        void Shutdown();

        void Enqueue(TaskSet&& ts);
        void Enqueue(Task&& t);

        // The calling thread dequeues task until task queue becomes empty
        void PumpUntilEmpty();
        // Waits until all tasks are finished (!= empty queue)
        bool TryFlush();

        ZetaInline bool AreAllTasksFinished() const
        {
            const bool isEmpty = m_numTasksFinished.load(std::memory_order_acquire) == 
                m_numTasksToFinishTarget.load(std::memory_order_acquire);
            return isEmpty;
        }

        ZetaInline int ThreadPoolSize() const { return m_threadPoolSize; }

    private:
        void WorkerThread(int idx);

        int m_threadPoolSize;
        int m_totalNumThreads;
        std::atomic_int32_t m_numTasksInQueue = 0;
        std::atomic_int32_t m_numTasksFinished = 0;
        std::atomic_int32_t m_numTasksToFinishTarget = 0;

        std::thread m_threadPool[MAX_NUM_THREADS];

        // Concurrent task queue
        // Source: https://github.com/cameron314/concurrentqueue
        struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
        {
            static const size_t BLOCK_SIZE = 256;
        };

        moodycamel::BlockingConcurrentQueue<Task, MyTraits> m_taskQueue;

        alignas(alignof(moodycamel::ProducerToken)) uint8_t m_producerTokensMem[
            sizeof(moodycamel::ProducerToken) * MAX_NUM_THREADS];
        moodycamel::ProducerToken* m_producerTokens;

        alignas(alignof(moodycamel::ConsumerToken)) uint8_t m_consumerTokensMem[
            sizeof(moodycamel::ConsumerToken) * MAX_NUM_THREADS];
        moodycamel::ConsumerToken* m_consumerTokens;

        std::atomic_bool m_start = false;
        std::atomic_bool m_shutdown = false;
    };
}
