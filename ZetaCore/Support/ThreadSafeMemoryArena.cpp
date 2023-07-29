#include "ThreadSafeMemoryArena.h"
#include "../App/App.h"
#include "../Utility/Span.h"
#include <thread>
#include <bit>

using namespace ZetaRay::Support;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// ThreadSafeMemoryArena
//--------------------------------------------------------------------------------------

ThreadSafeMemoryArena::ThreadSafeMemoryArena(size_t blockSize, int initNumBlocks) noexcept
	: k_defaultBlockSize(blockSize)
{
	auto threadIDs = App::GetAllThreadIDs();

	// cache the number of threads & their IDs
	m_numThreads = (int)threadIDs.size();

	for (int i = 0; i < threadIDs.size(); i++)
		m_threadIDs[i] = threadIDs[i];

	// zero allocations per thread initially
	for (int i = 0; i < threadIDs.size(); i++)
		m_threadCurrBlockIdx[i] = -1;

	m_blocks.resize(initNumBlocks);
	m_blocksLock = SRWLOCK_INIT;

	m_currBlockIdx.store(0, std::memory_order_release);
}

void* ThreadSafeMemoryArena::AllocateAligned(size_t size, size_t alignment) noexcept
{
	alignment = Math::Max(alignof(std::max_align_t), alignment);

	// at most alignment - 1 extra bytes are required
	const bool defaultBlockSizeEnough = size + alignment - 1 <= k_defaultBlockSize;

	int threadIdx = -1;
	const ZETA_THREAD_ID_TYPE currThreadID = std::bit_cast<ZETA_THREAD_ID_TYPE, std::thread::id>(std::this_thread::get_id());

	for (int i = 0; i < m_numThreads; i++)
	{
		if (m_threadIDs[i] == currThreadID)
		{
			threadIdx = i;
			break;
		}
	}

	Assert(threadIdx != -1, "thread idx was not found.");
	int blockIdx = m_threadCurrBlockIdx[threadIdx];

	// not the first time for this thread
	if (blockIdx != -1)
	{
		MemoryBlock& block = m_blocks[blockIdx];

		const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
		const uintptr_t ret = Math::AlignUp(start + block.Offset, alignment);
		const uintptr_t startOffset = ret - start;

		if (startOffset + size < block.Size)
		{
			block.Offset = startOffset + size;
			return reinterpret_cast<void*>(ret);
		}
	}

	// allocate a new block
	blockIdx = m_currBlockIdx.fetch_add(1, std::memory_order_relaxed);
	m_threadCurrBlockIdx[threadIdx] = blockIdx;

	// this should be rare -- only needed when there's no more free memory in this thread's block. Also,
	// assuming block sizes are not too small, contention should be low
	if (blockIdx >= m_blocks.size())
	{
		AcquireSRWLockExclusive(&m_blocksLock);
		m_blocks.resize(blockIdx + 1);
		ReleaseSRWLockExclusive(&m_blocksLock);
	}

	MemoryBlock& block = m_blocks[blockIdx];
	block.Size = defaultBlockSizeEnough ? k_defaultBlockSize : Math::NextPow2(size);
	block.Start = malloc(block.Size);
	Check(block.Start, "malloc() of %llu kbytes failed.", block.Size / 1024);

	const uintptr_t start = reinterpret_cast<uintptr_t>(block.Start);
	const uintptr_t ret = Math::AlignUp(start, alignment);
	const uintptr_t startOffset = ret - start;

	Assert(startOffset + size < block.Size, "should never happen.");
	block.Offset = startOffset + size;

	return reinterpret_cast<void*>(ret);
}

size_t ThreadSafeMemoryArena::TotalSizeInBytes() noexcept
{
	size_t sum = 0;

	for (auto& block : m_blocks)
		sum += block.Size;

	return sum;
}
