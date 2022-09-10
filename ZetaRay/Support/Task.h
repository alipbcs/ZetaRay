#pragma once

#include "../Utility/Error.h"
#include "../Utility/Span.h"
#include "../Utility/Function.h"
#include <atomic>

namespace ZetaRay::Support
{
	enum class TASK_PRIORITY
	{
		NORMAL,
		BACKGRUND
	};

	//--------------------------------------------------------------------------------------
	// Task
	//--------------------------------------------------------------------------------------

	struct TaskSet;

	struct alignas(64) Task
	{
		friend struct TaskSet;
		static constexpr int MAX_NAME_LENGTH = 64;

		Task() noexcept = default;
		Task(const char* name, TASK_PRIORITY p, Util::Function&& f) noexcept;
		~Task() noexcept = default;

		Task(Task&&) noexcept;
		Task& operator=(Task&&) noexcept;

		void Reset(const char* name, TASK_PRIORITY p, Util::Function&& f) noexcept;
		const char* GetName() const { return m_name; }
		int GetSignalHandle() const { return m_signalHandle; }
		Util::Span<int> GetAdjacencies() { return Util::Span(m_adjacentTailNodes); }
		TASK_PRIORITY GetPriority() { return m_priority; }

		__forceinline void DoTask() noexcept
		{
			Assert(m_dlg.IsSet(), "attempting to run an empty Function");
			m_dlg.Run();
		}

	private:
		Util::Function m_dlg;
		Util::SmallVector<int> m_adjacentTailNodes;
		char m_name[MAX_NAME_LENGTH];
		int m_signalHandle = -1;
		int m_indegree = 0;
		TASK_PRIORITY m_priority;
	};

	//--------------------------------------------------------------------------------------
	// WaitObject
	//--------------------------------------------------------------------------------------

	struct WaitObject
	{
		WaitObject() = default;

		void Notify() noexcept
		{
			m_completionFlag.store(true, std::memory_order_release);
			m_completionFlag.notify_one();
		}

		void Wait() noexcept
		{
			m_completionFlag.wait(false, std::memory_order_relaxed);
		}

	private:
		std::atomic_bool m_completionFlag = false;
	};

	//--------------------------------------------------------------------------------------
	// TaskSet
	//--------------------------------------------------------------------------------------

	// Intented for usage by a single thread.
	// Usage:
	// 1. Add Tasks (EmplaceTask())
	// 2. Add intra-TaskSet edges (AddOutgoingEdge())
	// 3. Sort
	// 4. Connect different TaskSets
	// 5. Finalize
	struct TaskSet
	{
		//static constexpr int MAX_NUM_TASKS = sizeof(uint64_t) * 8;
		static constexpr int MAX_NUM_TASKS = 20;
		using TaskHandle = int;

		TaskSet() noexcept = default;
		~TaskSet() noexcept = default;

		TaskSet(const TaskSet&) = delete;
		TaskSet& operator=(const TaskSet&) = delete;

		inline TaskHandle EmplaceTask(const char* name, Util::Function&& f) noexcept
		{
			Check(!m_isFinalized, "Calling AddTask() on an unfinalized TaskSet is not allowed.");
			Check(m_currSize < MAX_NUM_TASKS - 2, "current implementation of this functions doesn't support more than 64 tasks.");

			// TaskSet is not needed for background tasks
			m_tasks[m_currSize++].Reset(name, TASK_PRIORITY::NORMAL, ZetaMove(f));

			return (TaskHandle)(m_currSize - 1);
		}
		
		// Adds a dependant task to the list of tasks that are notified by this task upon completion
		void AddOutgoingEdge(TaskHandle a, TaskHandle b) noexcept;
		// Adds an edge from given task to every other task that is "currently" is the TaskSet
		void AddOutgoingEdgeToAll(TaskHandle a) noexcept;
		void AddIncomingEdgeFromAll(TaskHandle a) noexcept;
		void ConnectTo(TaskSet& other) noexcept;
		void ConnectTo(Task& other) noexcept;
		void ConnectFrom(Task& other) noexcept;

		bool IsFinalized() noexcept { return m_isFinalized; }
		void Sort() noexcept;
		void Finalize(WaitObject* waitObj = nullptr) noexcept;

		int GetSize() { return m_currSize; }
		Util::Span<Task> GetTasks() { return Util::Span(m_tasks, m_currSize); }

	private:
		struct TaskMetadata
		{
			int Indegree() const { return __popcnt16(PredecessorMask); }
			int Outdegree() const { return __popcnt16(SuccessorMask); }
			
			// index of adjacet tasks (this Task has an edge to them)
			uint16_t SuccessorMask = 0;

			// index of predecessor nodes (have an edge to this Task)
			uint16_t PredecessorMask = 0;
		};

		void ComputeInOutMask() noexcept;
		void TopologicalSort() noexcept;

		Task m_tasks[MAX_NUM_TASKS];
		TaskMetadata m_taskMetadata[MAX_NUM_TASKS];

		uint16_t m_rootMask = 0;
		uint16_t m_leafMask = 0;

		uint8_t m_currSize = 0;
		bool m_isSorted = false;
		bool m_isFinalized = false;
	};
}

