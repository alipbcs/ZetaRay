#include "Task.h"
#include "../App/Timer.h"
#include <intrin.h>

using namespace ZetaRay::Support;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// Task
//--------------------------------------------------------------------------------------

Task::Task(const char* name, TASK_PRIORITY p, Function&& f) noexcept
	: m_priority(p)
{
	int n = stbsp_snprintf(m_name, MAX_NAME_LENGTH, "Frame %u | %s", App::GetTimer().GetTotalFrameCount(), name);
	//Assert(n < MAX_NAME_LENGTH, "not enough space in buffer");

	m_dlg = ZetaMove(f);

	if(p == TASK_PRIORITY::NORMAL)
		m_signalHandle = App::RegisterTask();
}

Task::Task(Task&& other) noexcept
{
	if (this == &other)
		return;

	//m_adjacentTailNodes.swap(other.m_adjacentTailNodes);
	m_adjacentTailNodes = ZetaMove(other.m_adjacentTailNodes);
	other.m_adjacentTailNodes.clear();

	m_dlg = ZetaMove(other.m_dlg);
	std::swap(m_indegree, other.m_indegree);
	std::swap(m_signalHandle, other.m_signalHandle);
	m_priority = other.m_priority;
	other.m_signalHandle = -1;

	memcpy(m_name, other.m_name, MAX_NAME_LENGTH);
	memset(other.m_name, '\0', MAX_NAME_LENGTH);

	//m_indegreeAtomic.store(other.m_indegreeAtomic.load(std::memory_order_relaxed));
	//m_blockFlag.store(false, std::memory_order_relaxed);
}

Task& Task::operator=(Task&& other) noexcept
{
	//m_adjacentTailNodes.swap(other.m_adjacentTailNodes);
	m_adjacentTailNodes = ZetaMove(other.m_adjacentTailNodes);
	other.m_adjacentTailNodes.clear();

	m_dlg = ZetaMove(other.m_dlg);
	std::swap(m_indegree, other.m_indegree);
	std::swap(m_signalHandle, other.m_signalHandle);
	m_priority = other.m_priority;
	other.m_signalHandle = -1;

	memcpy(m_name, other.m_name, MAX_NAME_LENGTH);
	memset(other.m_name, '\0', MAX_NAME_LENGTH);

	//m_indegreeAtomic.store(other.m_indegreeAtomic.load(std::memory_order_relaxed));
	//m_blockFlag.store(false, std::memory_order_relaxed);

	return *this;
}

void Task::Reset(const char* name, TASK_PRIORITY p, Function&& f) noexcept
{
	m_priority = p;
	Check(m_signalHandle == -1, "Reinitialization is not allowed.");

	int n = stbsp_snprintf(m_name, MAX_NAME_LENGTH, "Frame %llu | %s", App::GetTimer().GetTotalFrameCount(), name);
	Assert(n < MAX_NAME_LENGTH, "not enough space in buffer");
	m_indegree = 0;
	//m_blockFlag.store(true, std::memory_order_relaxed);
	//m_indegreeAtomic.store(0, std::memory_order_relaxed);

	m_dlg = ZetaMove(f);

	if(p == TASK_PRIORITY::NORMAL)
		m_signalHandle = App::RegisterTask();
}

//--------------------------------------------------------------------------------------
// TaskSet
//--------------------------------------------------------------------------------------

void TaskSet::AddOutgoingEdge(TaskHandle a, TaskHandle b) noexcept
{
	Assert(a < m_currSize && b < m_currSize, "Invalid task handles");

	TaskMetadata& ta = m_taskMetadata[a];
	TaskMetadata& tb = m_taskMetadata[b];

	bool prev1 = _bittestandset((long*)&ta.SuccessorMask, b);
	Assert(!prev1, "Reduntant call. Edge had already beed added.");

	bool prev2 = _bittestandset((long*)&tb.PredecessorMask, a);
	Assert(!prev2, "Reduntant call. Edge had already beed added.");

	m_tasks[a].m_adjacentTailNodes.push_back(m_tasks[b].m_signalHandle);
}

void TaskSet::AddOutgoingEdgeToAll(TaskHandle a) noexcept
{
	Assert(a < m_currSize, "Invalid task handle");
	TaskMetadata& ta = m_taskMetadata[a];

	for (int b = 0; b < m_currSize; b++)
	{
		if (b == a)
			continue;

		TaskMetadata& tb = m_taskMetadata[b];

		ta.SuccessorMask |= (1 << b);
		tb.PredecessorMask |= (1 << a);

		m_tasks[a].m_adjacentTailNodes.push_back(m_tasks[b].m_signalHandle);
	}
}

void TaskSet::AddIncomingEdgeFromAll(TaskHandle a) noexcept
{
	Assert(a < m_currSize, "Invalid task handle");
	TaskMetadata& ta = m_taskMetadata[a];

	for (int b = 0; b < m_currSize; b++)
	{
		if (b == a)
			continue;

		TaskMetadata& tb = m_taskMetadata[b];
		
		ta.PredecessorMask |= (1 << b);
		tb.SuccessorMask |= (1 << a);

		m_tasks[b].m_adjacentTailNodes.push_back(m_tasks[a].m_signalHandle);
	}
}

void TaskSet::Sort() noexcept
{
	Check(!m_isSorted, "Invalid call.");

	ComputeInOutMask();
	TopologicalSort();

	m_isSorted = true;
}

void TaskSet::Finalize(WaitObject* waitObj) noexcept
{
	Check(!m_isFinalized && m_isSorted, "Invalid call.");

	for (int i = 0; i < m_currSize; i++)
	{
		int indegree = m_taskMetadata[i].Indegree();
		// deps between tasksets can't be detected by indegree as those solely
		// correspond to deps inside the taskset
		if (indegree > 0 || m_tasks[i].m_indegree > 0)
		{
			m_tasks[i].m_indegree = Math::max(indegree, m_tasks[i].m_indegree);
			// only need to register tasks that have indegree > 0
			App::TaskFinalizedCallback(m_tasks[i].m_signalHandle, m_tasks[i].m_indegree);
		}
	}

	m_isFinalized = true;

	if (waitObj)
	{
		Assert(m_currSize < MAX_NUM_TASKS, "no more space for new tasks in this TaskSet.");
		m_tasks[m_currSize++].Reset("NotifyCompletion", m_tasks[0].m_priority, [waitObj]()
			{
				waitObj->Notify();
			});

		// ConnectTo(m_tasks[m_currSize]);
		Task& notifyTask = m_tasks[m_currSize - 1];
		uint64_t mask = m_leafMask;
		notifyTask.m_indegree += __popcnt16((uint16_t)mask);

		unsigned long idx;
		while (_BitScanForward64(&idx, mask))
		{
			Assert(idx < m_currSize, "Bug");
			m_tasks[idx].m_adjacentTailNodes.push_back(notifyTask.m_signalHandle);

			mask &= ~(1llu << idx);
		}

		App::TaskFinalizedCallback(notifyTask.m_signalHandle, notifyTask.m_indegree);
	}
}

void TaskSet::ComputeInOutMask() noexcept
{
	for (int i = 0; i < m_currSize; ++i)
	{
		if (m_taskMetadata[i].Indegree() == 0)
			m_rootMask |= (1ull << i);

		if (m_taskMetadata[i].Outdegree() == 0)
			m_leafMask |= (1ull << i);
	}
}

void TaskSet::TopologicalSort() noexcept
{
	// at each itertation, points to remaining elements that have an indegree of 0
	uint64_t currMask = m_rootMask;
	size_t currIdx = 0;	
	int sorted[MAX_NUM_TASKS];

	// make a temporary copy of indegrees for topological sorting
	int tempIndegree[MAX_NUM_TASKS];
	for (int i = 0; i < m_currSize; i++)
	{
		tempIndegree[i] = m_taskMetadata[i].Indegree();
	}

	// find all the nodes with indegree == 0
	unsigned long zeroIndegreeIdx;
	while (_BitScanForward64(&zeroIndegreeIdx, currMask))
	{
		Assert(zeroIndegreeIdx < m_currSize, "Invalid index.");
		TaskMetadata& t = m_taskMetadata[zeroIndegreeIdx];
		uint64_t tails = t.SuccessorMask;
		unsigned long tailIdx;

		// for every tail-adjacent node
		while (_BitScanForward64(&tailIdx, tails))
		{
			Assert(tailIdx < m_currSize, "Invalid index.");

			// remove one edge
			tempIndegree[tailIdx] -= 1;

			// if tail node's indegree has become 0, add it to mask
			if (tempIndegree[tailIdx] == 0)
				currMask |= (1llu << tailIdx);

			tails &= ~(1llu << tailIdx);
		}

		// save the new position for current nodes
		sorted[currIdx++] = zeroIndegreeIdx;

		// remove current node
		currMask &= ~(1llu << zeroIndegreeIdx);
	}

	Assert(currIdx == m_currSize, "bug");

	for (int i = 0; i < m_currSize; i++)
		Assert(tempIndegree[i] == 0, "Graph has a cycle.");

	Task oldTaskArr[MAX_NUM_TASKS];
	for (int i = 0; i < m_currSize; i++)
	{
		oldTaskArr[i] = ZetaMove(m_tasks[i]);
	}

	TaskMetadata oldTaskMetadata[MAX_NUM_TASKS];
	memcpy(oldTaskMetadata, m_taskMetadata, m_currSize * sizeof(TaskMetadata));

	for (int i = 0; i < m_currSize; i++)
	{
		m_tasks[i] = ZetaMove(oldTaskArr[sorted[i]]);
		m_taskMetadata[i] = oldTaskMetadata[sorted[i]];
	}
}

void TaskSet::ConnectTo(TaskSet& other) noexcept
{
	Assert(!m_isFinalized, "Calling this method on a finalized TaskSet is invalid.");
	Assert(!other.m_isFinalized, "Calling this method on a finalized TaskSet is invalid.");

	uint64_t headMask = m_leafMask;
	uint64_t tailMask = other.m_rootMask;

	// connect every leaf of this TaskSet to every root of "other"
	unsigned long headIdx;
	while (_BitScanForward64(&headIdx, headMask))
	{
		Assert(headIdx < m_currSize, "Bug");
		Assert(m_tasks[headIdx].m_adjacentTailNodes.empty(), "Leaf task should not have tail nodes.");
		m_tasks[headIdx].m_adjacentTailNodes.reserve(__popcnt16(other.m_rootMask));

		unsigned long tailIdx;
		while (_BitScanForward64(&tailIdx, tailMask))
		{
			Assert(tailIdx < other.m_currSize, "Index out of bound.");

			// add one edge
			other.m_tasks[tailIdx].m_indegree += 1;
			m_tasks[headIdx].m_adjacentTailNodes.push_back(other.m_tasks[tailIdx].m_signalHandle);

			tailMask &= ~(1 << tailIdx);
		}

		headMask &= ~(1 << headIdx);
	}
}

void TaskSet::ConnectTo(Task& other) noexcept
{
	Assert(!m_isFinalized, "Calling this method on a finalized TaskSet is invalid.");

	uint64_t mask = m_leafMask;

	unsigned long idx;
	while (_BitScanForward64(&idx, mask))
	{
		Assert(idx < m_currSize, "Bug");
		m_tasks[idx].m_adjacentTailNodes.push_back(other.m_signalHandle);

		mask &= ~(1llu << idx);
	}

	other.m_indegree += __popcnt16((uint16_t)mask);
}

void TaskSet::ConnectFrom(Task& other) noexcept
{
	Assert(!m_isFinalized, "Calling this method on a finalized TaskSet is invalid.");

	uint64_t mask = m_rootMask;

	unsigned long idx;
	while (_BitScanForward64(&idx, mask))
	{
		Assert(idx < m_currSize, "Invalid index.");
		m_tasks[idx].m_indegree += 1;
		other.m_adjacentTailNodes.push_back(m_tasks[idx].m_signalHandle);

		mask &= ~(1llu << idx);
	}
}