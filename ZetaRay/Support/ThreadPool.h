#pragma once

#include "Task.h"
#include "concurrentqueue-1.0.3/blockingconcurrentqueue.h"

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
		ThreadPool() noexcept = default;
		~ThreadPool() noexcept = default;

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;

		// create the threads, after which threads are waiting for tasks to exectute, also registers for thread memory pool
		void Init(int poolSize, int totalNumThreads, const wchar_t* threadNamePrefix, THREAD_PRIORITY p) noexcept;
		//void SetThreadIds(Span<std::thread::id> allThreadIds) noexcept;
		void Start() noexcept;

		// signals the shutdown flag
		void Shutdown() noexcept;

		// Enqueues tasks
		void Enqueue(TaskSet&& ts) noexcept;
		void Enqueue(Task&& t) noexcept;

		// Calling thread (usaully main) dequeues task until queue becomes empty
		void PumpUntilEmpty() noexcept;

		// Wait until are tasks are "finished" (!= empty queue)
		bool TryFlush() noexcept;

		inline bool IsEmpty() noexcept
		{
			const bool isEmpty = m_numTasksFinished.load(std::memory_order_acquire) == m_numTasksToFinishTarget.load(std::memory_order_acquire);
			return isEmpty;
		}

		int ThreadPoolSize() const { return m_threadPoolSize; }
		Util::Span<std::thread::id> ThreadIDs() { return Util::Span(m_threadIDs, m_threadPoolSize); }

	private:
		void WorkerThread() noexcept;
		
		int m_threadPoolSize;
		int m_totalNumThreads;

		std::atomic_int32_t m_numTasksInQueue = 0;
		std::atomic_int32_t m_numTasksFinished = 0;
		std::atomic_int32_t m_numTasksToFinishTarget = 0;

		// thread pool
		std::thread m_threadPool[MAX_NUM_THREADS];
		std::thread::id m_threadIDs[MAX_NUM_THREADS];
		uint32_t m_appThreadIds[MAX_NUM_THREADS];
		
		// concurrent task queue
		// Source: https://github.com/cameron314/concurrentqueue
		struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
		{
			static const size_t BLOCK_SIZE = 256;
		};

		moodycamel::BlockingConcurrentQueue<Task, MyTraits> m_taskQueue;

		alignas(alignof(moodycamel::ProducerToken)) uint8_t m_producerTokensMem[sizeof(moodycamel::ProducerToken) * MAX_NUM_THREADS];
		moodycamel::ProducerToken* m_producerTokens;
		
		alignas(alignof(moodycamel::ConsumerToken)) uint8_t m_consumerTokensMem[sizeof(moodycamel::ConsumerToken) * MAX_NUM_THREADS];
		moodycamel::ConsumerToken* m_consumerTokens;

		std::atomic_bool m_start = false;
		std::atomic_bool m_shutdown = false;
	};
}
