#pragma once

#include "Task.h"
#include "concurrentqueue/blockingconcurrentqueue.h"

namespace ZetaRay::Support
{
    enum class THREAD_PRIORITY
    {
        NORMAL,
        BACKGROUND
    };

    class ThreadPool
    {
    public:
        ThreadPool() = default;
        ~ThreadPool() = default;

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        // create the threads, after which threads are waiting for tasks to exectute, also registers for thread memory pool
        void Init(int poolSize, int totalNumThreads, const wchar_t* threadNamePrefix, THREAD_PRIORITY p);
        //void SetThreadIds(Span<std::thread::id> allThreadIds);
        void Start();

        // signals the shutdown flag
        void Shutdown();

        // Enqueues tasks
        void Enqueue(TaskSet&& ts);
        void Enqueue(Task&& t);

        // Calling thread (usaully main) dequeues task until queue becomes empty
        void PumpUntilEmpty();

        // Wait until are tasks are "finished" (!= empty queue)
        bool TryFlush();

        bool AreAllTasksFinished()
        {
            const bool isEmpty = m_numTasksFinished.load(std::memory_order_acquire) == m_numTasksToFinishTarget.load(std::memory_order_acquire);
            return isEmpty;
        }

        ZetaInline int ThreadPoolSize() const { return m_threadPoolSize; }
        ZetaInline Util::Span<std::thread::id> ThreadIDs() { return Util::Span(m_threadIDs, m_threadPoolSize); }

    private:
        void WorkerThread();

        int m_threadPoolSize;
        int m_totalNumThreads;

        std::atomic_int32_t m_numTasksInQueue = 0;
        std::atomic_int32_t m_numTasksFinished = 0;
        std::atomic_int32_t m_numTasksToFinishTarget = 0;

        // thread pool
        std::thread m_threadPool[ZETA_MAX_NUM_THREADS];
        std::thread::id m_threadIDs[ZETA_MAX_NUM_THREADS];
        ZETA_THREAD_ID_TYPE m_appThreadIds[ZETA_MAX_NUM_THREADS];

        // concurrent task queue
        // Source: https://github.com/cameron314/concurrentqueue
        struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
        {
            static const size_t BLOCK_SIZE = 256;
        };

        moodycamel::BlockingConcurrentQueue<Task, MyTraits> m_taskQueue;

        alignas(alignof(moodycamel::ProducerToken)) uint8_t m_producerTokensMem[sizeof(moodycamel::ProducerToken) * ZETA_MAX_NUM_THREADS];
        moodycamel::ProducerToken* m_producerTokens;

        alignas(alignof(moodycamel::ConsumerToken)) uint8_t m_consumerTokensMem[sizeof(moodycamel::ConsumerToken) * ZETA_MAX_NUM_THREADS];
        moodycamel::ConsumerToken* m_consumerTokens;

        std::atomic_bool m_start = false;
        std::atomic_bool m_shutdown = false;
    };
}
