#pragma once

#include "../Utility/Error.h"
#include "../Utility/Span.h"
#include "../Utility/Function.h"
#include "../App/App.h"
#include <atomic>

#define USE_TASK_NAMES 0

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

		Task() = default;
		Task(const char* name, TASK_PRIORITY p, Util::Function&& f);
		~Task() = default;

		Task(Task&&);
		Task& operator=(Task&&);

		void Reset(const char* name, TASK_PRIORITY p, Util::Function&& f);
#if USE_TASK_NAMES == 1
		ZetaInline const char* GetName() const { return m_name; }
#endif
		ZetaInline int GetSignalHandle() const { return m_signalHandle; }
		ZetaInline Util::Span<int> GetAdjacencies() { return Util::Span(m_adjacentTailNodes); }
		ZetaInline TASK_PRIORITY GetPriority() { return m_priority; }

		ZetaInline void DoTask()
		{
			Assert(m_dlg.IsSet(), "attempting to run an empty Function");
			m_dlg.Run();
		}

	private:
		Util::Function m_dlg;
		Util::SmallVector<int, App::FrameAllocator> m_adjacentTailNodes;

#if USE_TASK_NAMES == 1
		char m_name[MAX_NAME_LENGTH];
#endif
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

		void Notify()
		{
			m_completionFlag.store(true, std::memory_order_release);
			m_completionFlag.notify_one();
		}

		void Wait()
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
	// 
	// Usage:
	// 
	// 1. Add Tasks (EmplaceTask())
	// 2. Add intra-TaskSet edges (AddOutgoingEdge())
	// 3. Sort
	// 4. Connect different TaskSets
	// 5. Finalize
	struct TaskSet
	{
		//static constexpr int MAX_NUM_TASKS = sizeof(uint64_t) * 8;
		static constexpr int MAX_NUM_TASKS = 18;
		using TaskHandle = int;

		TaskSet() = default;
		~TaskSet() = default;

		TaskSet(const TaskSet&) = delete;
		TaskSet& operator=(const TaskSet&) = delete;

		TaskHandle EmplaceTask(const char* name, Util::Function&& f)
		{
			Check(!m_isFinalized, "Calling AddTask() on an unfinalized TaskSet is not allowed.");
			Check(m_currSize < MAX_NUM_TASKS - 2, "current implementation of this functions doesn't support more than 64 tasks.");

			// TaskSet is not needed for background tasks
			m_tasks[m_currSize++].Reset(name, TASK_PRIORITY::NORMAL, ZetaMove(f));

			return (TaskHandle)(m_currSize - 1);
		}
		
		// Adds a dependent task to the list of tasks that are notified by this task upon completion
		void AddOutgoingEdge(TaskHandle a, TaskHandle b);
		// Adds an edge from the given task to every other task that is "currently" is the TaskSet
		void AddOutgoingEdgeToAll(TaskHandle a);
		void AddIncomingEdgeFromAll(TaskHandle a);
		void ConnectTo(TaskSet& other);
		void ConnectTo(Task& other);
		void ConnectFrom(Task& other);

		ZetaInline bool IsFinalized() { return m_isFinalized; }
		void Sort();
		void Finalize(WaitObject* waitObj = nullptr);

		ZetaInline int GetSize() { return m_currSize; }
		ZetaInline Util::MutableSpan<Task> GetTasks() { return Util::MutableSpan(m_tasks, m_currSize); }

	private:
		struct TaskMetadata
		{
			ZetaInline int Indegree() const { return __popcnt16(PredecessorMask); }
			ZetaInline int Outdegree() const { return __popcnt16(SuccessorMask); }
			
			// index of adjacent tasks (this Task has an edge to them)
			uint16_t SuccessorMask = 0;

			// index of predecessor tasks (have an edge to this Task)
			uint16_t PredecessorMask = 0;
		};

		void ComputeInOutMask();
		void TopologicalSort();

		Task m_tasks[MAX_NUM_TASKS];
		TaskMetadata m_taskMetadata[MAX_NUM_TASKS];

		uint16_t m_rootMask = 0;
		uint16_t m_leafMask = 0;

		uint8_t m_currSize = 0;
		bool m_isSorted = false;
		bool m_isFinalized = false;
	};
}

