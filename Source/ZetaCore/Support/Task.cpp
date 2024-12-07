#include "Task.h"
#include "../App/Timer.h"
#include <intrin.h>

using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// Task
//--------------------------------------------------------------------------------------

Task::Task(const char* name, TASK_PRIORITY priority, Function&& f)
    : m_dlg(ZetaMove(f)),
    m_priority(priority)
{
    if(m_priority == TASK_PRIORITY::NORMAL)
        m_signalHandle = App::RegisterTask();
}

Task::Task(Task&& other)
    : m_dlg(ZetaMove(other.m_dlg)),
    m_signalHandle(other.m_signalHandle),
    m_indegree(other.m_indegree),
    m_priority(other.m_priority)
{
    //m_adjacentTailNodes.swap(other.m_adjacentTailNodes);
    m_adjacentTailNodes = ZetaMove(other.m_adjacentTailNodes);
    other.m_adjacentTailNodes.clear();

    other.m_indegree = 0;
    other.m_signalHandle = -1;
}

Task& Task::operator=(Task&& other)
{
    if (this == &other)
        return *this;

    //m_adjacentTailNodes.swap(other.m_adjacentTailNodes);
    m_adjacentTailNodes = ZetaMove(other.m_adjacentTailNodes);
    other.m_adjacentTailNodes.clear();

    m_dlg = ZetaMove(other.m_dlg);
    m_indegree = other.m_indegree;
    m_signalHandle = other.m_signalHandle;
    m_priority = other.m_priority;
    other.m_indegree = 0;
    other.m_signalHandle = -1;

    return *this;
}

void Task::Reset(const char* name, TASK_PRIORITY priority, Function&& f)
{
    Assert(m_signalHandle == -1, "Reinitialization is not allowed.");

    m_priority = priority;
    m_indegree = 0;
    m_dlg = ZetaMove(f);

    if(m_priority == TASK_PRIORITY::NORMAL)
        m_signalHandle = App::RegisterTask();
}

//--------------------------------------------------------------------------------------
// TaskSet
//--------------------------------------------------------------------------------------

void TaskSet::AddOutgoingEdge(TaskHandle a, TaskHandle b)
{
    Assert(a < m_currSize && b < m_currSize, "Invalid task handles.");
    TaskMetadata& ta = m_taskMetadata[a];
    TaskMetadata& tb = m_taskMetadata[b];

    bool prev1 = _bittestandset((long*)&ta.SuccessorMask, b);
    Assert(!prev1, "Redundant call, edge already exists.");

    bool prev2 = _bittestandset((long*)&tb.PredecessorMask, a);
    Assert(!prev2, "Redundant call, edge already exists.");

    m_tasks[a].m_adjacentTailNodes.push_back(m_tasks[b].m_signalHandle);
}

void TaskSet::AddOutgoingEdgeToAll(TaskHandle a)
{
    Assert(a < m_currSize, "Invalid task handle.");
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

void TaskSet::AddIncomingEdgeFromAll(TaskHandle a)
{
    Assert(a < m_currSize, "Invalid task handle.");
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

void TaskSet::Sort()
{
    Assert(!m_isSorted, "TaskSet is already sorted.");
    TopologicalSort();
    ComputeInOutMask();

    m_isSorted = true;
}

void TaskSet::Finalize(WaitObject* waitObj)
{
    Assert(!m_isFinalized && m_isSorted, "Finalize() shouldn't be called when TaskSet hasn't been sorted.");

    for (int i = 0; i < m_currSize; i++)
    {
        const int indegree = m_taskMetadata[i].Indegree();

        // Dependencies between TaskSets can't be detected by indegree as those only
        // for dependencies inside the TaskSet
        if (indegree > 0 || m_tasks[i].m_indegree > 0)
        {
            // Dependencies between TaskSets only increase the indegree
            // for root nodes (which have indegree of 0 inside the taskset)
            Assert(indegree * m_tasks[i].m_indegree == 0, "bug");
            m_tasks[i].m_indegree = Max(indegree, m_tasks[i].m_indegree);

            // Only need to register tasks that have indegree > 0
            App::TaskFinalizedCallback(m_tasks[i].m_signalHandle, m_tasks[i].m_indegree);
        }
    }

    m_isFinalized = true;

    if (waitObj)
    {
        Assert(m_currSize < MAX_NUM_TASKS, "Out of space for new tasks in this TaskSet.");
        m_tasks[m_currSize++].Reset("NotifyCompletion", m_tasks[0].m_priority, [waitObj]()
            {
                waitObj->Notify();
            });

        // ConnectTo(m_tasks[m_currSize]);
        Task& notifyTask = m_tasks[m_currSize - 1];
        uint64_t mask = m_leafMask;
        notifyTask.m_indegree += __popcnt((uint32_t)mask);

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

void TaskSet::ComputeInOutMask()
{
    for (int i = 0; i < m_currSize; ++i)
    {
        if (m_taskMetadata[i].Indegree() == 0)
            m_rootMask |= (1llu << i);

        if (m_taskMetadata[i].Outdegree() == 0)
            m_leafMask |= (1llu << i);
    }
}

void TaskSet::TopologicalSort()
{
    // Find the root nodes
    uint32_t rootMask = 0;

    for (int i = 0; i < m_currSize; ++i)
    {
        if (m_taskMetadata[i].Indegree() == 0)
            rootMask |= (1llu << i);
    }

    // In each iteration, points to remaining elements that have an indegree of zero
    uint32_t currMask = rootMask;
    size_t currIdx = 0;
    int sorted[MAX_NUM_TASKS];

    // Make a temporary copy for the duration of topological sorting
    int tempIndegree[MAX_NUM_TASKS];
    for (int i = 0; i < m_currSize; i++)
        tempIndegree[i] = m_taskMetadata[i].Indegree();

    // Find all nodes with zero indegree
    unsigned long zeroIndegreeIdx;
    while (_BitScanForward64(&zeroIndegreeIdx, currMask))
    {
        Assert(zeroIndegreeIdx < m_currSize, "Invalid index.");
        TaskMetadata& t = m_taskMetadata[zeroIndegreeIdx];
        uint32_t tails = t.SuccessorMask;
        unsigned long tailIdx;

        // For every tail-adjacent node
        while (_BitScanForward64(&tailIdx, tails))
        {
            Assert(tailIdx < m_currSize, "Invalid index.");

            // Remove one edge
            tempIndegree[tailIdx] -= 1;

            // If tail node's indegree has become 0, add it to mask
            if (tempIndegree[tailIdx] == 0)
                currMask |= (1 << tailIdx);

            tails &= ~(1 << tailIdx);
        }

        // Save new position for current node
        sorted[currIdx++] = zeroIndegreeIdx;

        // Remove current node
        currMask &= ~(1 << zeroIndegreeIdx);
    }

    Assert(currIdx == m_currSize, "Graph has a cycle.");

    for (int i = 0; i < m_currSize; i++)
        Assert(tempIndegree[i] == 0, "Graph has a cycle.");

    Task oldTaskArr[MAX_NUM_TASKS];
    for (int i = 0; i < m_currSize; i++)
        oldTaskArr[i] = ZetaMove(m_tasks[i]);

    TaskMetadata oldTaskMetadata[MAX_NUM_TASKS];
    memcpy(oldTaskMetadata, m_taskMetadata, m_currSize * sizeof(TaskMetadata));

    for (int i = 0; i < m_currSize; i++)
    {
        m_tasks[i] = ZetaMove(oldTaskArr[sorted[i]]);
        m_taskMetadata[i] = oldTaskMetadata[sorted[i]];
    }
}

void TaskSet::ConnectTo(TaskSet& other)
{
    Assert(!m_isFinalized, "Calling this method on a finalized TaskSet is invalid.");
    Assert(!other.m_isFinalized, "Calling this method on a finalized TaskSet is invalid.");

    uint64_t headMask = m_leafMask;
    const uint64_t tailMask = other.m_rootMask;

    // Connect every leaf of this TaskSet to every root of "other"
    unsigned long headIdx;
    while (_BitScanForward64(&headIdx, headMask))
    {
        Assert(headIdx < m_currSize, "Bug");
        Assert(m_tasks[headIdx].m_adjacentTailNodes.empty(), "Leaf task should not have tail nodes.");
        m_tasks[headIdx].m_adjacentTailNodes.reserve(__popcnt16(other.m_rootMask));

        unsigned long tailIdx;
        uint64_t tailMaskCopy = tailMask;
        while (_BitScanForward64(&tailIdx, tailMaskCopy))
        {
            Assert(tailIdx < other.m_currSize, "Index out of bound.");

            // Add one edge
            other.m_tasks[tailIdx].m_indegree += 1;
            m_tasks[headIdx].m_adjacentTailNodes.push_back(other.m_tasks[tailIdx].m_signalHandle);

            tailMaskCopy &= ~(1llu << tailIdx);
        }

        headMask &= ~(1llu << headIdx);
    }
}

void TaskSet::ConnectTo(Task& other)
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

    other.m_indegree += __popcnt((uint32_t)mask);
}

void TaskSet::ConnectFrom(Task& other)
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