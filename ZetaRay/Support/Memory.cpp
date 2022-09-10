#include "Memory.h"
#include "../Utility/Error.h"
#include "../Math/Common.h"
#include <intrin.h>
#include <cstdio>
#include <string.h>

using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// MemoryArena
//--------------------------------------------------------------------------------------

MemoryArena::MemoryArena(size_t s) noexcept
	: m_size(s),
	m_offset(0)
{
	m_mem = malloc(s);
}

MemoryArena::~MemoryArena() noexcept
{
	if (m_mem)
		free(m_mem);
}

void* MemoryArena::AllocateAligned(size_t size, const char* name, int alignment) noexcept
{
	m_offset = Math::AlignUp(m_offset, alignment);
	
	if (m_offset + size < m_size)
	{
		uintptr_t ret = reinterpret_cast<uintptr_t>(m_mem) + m_offset;
		m_offset += size;
		return reinterpret_cast<void*>(ret);
	}

	Assert(false, "Insufficient memory");
	return nullptr;
}

void MemoryArena::FreeAligned(void* pMem, size_t size, const char* name, int alignment) noexcept
{
}

//--------------------------------------------------------------------------------------
// MemoryPool
//--------------------------------------------------------------------------------------

MemoryPool::~MemoryPool() noexcept
{
	Clear();
}

void MemoryPool::Init() noexcept
{
	for (size_t i = 0; i < POOL_COUNT; i++)
	{
		m_currHead[i] = nullptr;
		m_numMemoryBlocks[i] = 0;
		m_pools[i] = nullptr;

		//Grow(i);
	}
}

void MemoryPool::Clear() noexcept
{
	for (int i = 0; i < POOL_COUNT; i++)
	{
		if (m_pools[i])
		{
			for (int j = 0; j < m_numMemoryBlocks[i]; j++)
			{
				// free the pointer to heads of linked list for each memory block
				free(m_pools[i][j]);
			}

			// free the block itself
			free(m_pools[i]);

			m_pools[i] = nullptr;
			m_currHead[i] = nullptr;
		}
	}

	memset(m_numMemoryBlocks, 0, sizeof(size_t) * POOL_COUNT);
}

size_t MemoryPool::GetPoolIndexFromSize(size_t x) noexcept
{
	size_t s = Math::max(MIN_ALLOC_SIZE, Math::NextPow2(x));
	unsigned long idx;
	_BitScanForward64(&idx, s);

	return idx - INDEX_SHIFT;
}

void* MemoryPool::Allocate(size_t size) noexcept
{
	// use malloc for requests bigger than block size
	if (size > MAX_ALLOC_SIZE)
		return malloc(size);
	
	// Which memory pool does it live in?
	size_t poolIndex = GetPoolIndexFromSize(size);
	size_t chunkSize = GetChunkSizeFromPoolIndex(poolIndex);

	// if the pool for requested size is empty or has become full, add a new memory block consisting of 
	// chunks of size "chunkSize"
	if (!m_currHead[poolIndex])
		Grow(poolIndex);

	// get the pointer to first entry in the linked list
	void* oldHead = m_currHead[poolIndex];
	// update head of the linked list
	memcpy(&m_currHead[poolIndex], m_currHead[poolIndex], sizeof(void*));

	return oldHead;
}

void* MemoryPool::AllocateAligned(size_t size, int alignment) noexcept
{
	Assert(alignment <= 256, "Alignment > 256 is not supported.");
	if (alignment <= alignof(std::max_align_t))
		return Allocate(size);

	// take alignment into account. 
	// Given alignment a, at most a - 1 additiona bytes are needed.
	// e.g. size = 1, alignment = 64 --> 63 more bytes has to be allocated
	// assuming memory for 1 bytes was allocated at an address that ended with 0x1.
	// 
	// Difference between the aligned addrees need to be saved so it can be undone 
	// when freeing the memory. Extrac alignment - 1 bytes provide the space to
	// store that. One corner case happens when the address is already aligned, 
	// which means there won't be any extra space between aligned and original address.
	// To handle this case, allocate alignment more bytes and always shift the 
	// orignal pointer to the next aligned addrees.
	//
	// Furthermore, in the worst case, only 1 byte can be used to store the difference,
	// so alignemnts up to 256 are supported (0 is interpreted as 256).
	const size_t maxNumBytes = size + alignment - 1 + 1;

	// Which memory pool does it live in?
	size_t poolIndex = GetPoolIndexFromSize(maxNumBytes);
	size_t chunkSize = GetChunkSizeFromPoolIndex(poolIndex);

	// use malloc for requests bigger than 512 bytes
	if (poolIndex >= POOL_COUNT)
	{
		Assert(size >= MAX_ALLOC_SIZE, "bug");
		//return malloc(size);
		return _aligned_malloc(size, alignment);
	}

	// if the pool for requested size is empty or has become full, add a new memory block consisting of 
	// chunks of size "chunkSize"
	if (!m_currHead[poolIndex])
		Grow(poolIndex);

	// get the pointer to first entry in the linked list
	void* head = m_currHead[poolIndex];
	void* oldHead = head;
	void* newHead = *reinterpret_cast<void**>(head);

	// update head of the linked list
	m_currHead[poolIndex] = newHead;

	// align the return pointer
	uintptr_t aligned = reinterpret_cast<uintptr_t>(oldHead);
	aligned = (aligned + alignment - 1) & ~(alignment - 1);

	// corner case described above
	if (aligned == reinterpret_cast<uintptr_t>(oldHead))
		aligned += alignment;

	ptrdiff_t diff = (aligned - reinterpret_cast<uintptr_t>(oldHead)) & 0xff;
	Assert(diff > 0 && diff <= 256, "Invalid difference between aligned and original pointer.");

	// store the difference
	memcpy(reinterpret_cast<void*>(aligned - 1), &diff, 1);

	return reinterpret_cast<void*>(aligned);
}

void MemoryPool::Free(void* pMem, size_t size) noexcept
{
	//free(pMem);

	if (pMem)
	{
		// this request was allocated with malloc
		if (size > MAX_ALLOC_SIZE)
		{
			free(pMem);
			return;
		}

		size_t poolIndex = GetPoolIndexFromSize(size);

		// set "mem"s next pointer to be current head of the linked list
		if (m_currHead[poolIndex])
			memcpy(pMem, &m_currHead[poolIndex], sizeof(void*));
		else
			memset(pMem, 0, sizeof(void*));

		// update the head of linked list to point to "pMem"
		m_currHead[poolIndex] = pMem;
	}
}

void MemoryPool::FreeAligned(void* mem, size_t size, int alignment) noexcept
{
	if (alignment <= alignof(std::max_align_t))
		return Free(mem, size);

	if (mem)
	{
		const size_t maxNumBytes = size + alignment - 1;

		// this request was allocated with malloc
		if (maxNumBytes > MAX_ALLOC_SIZE)
		{
			_aligned_free(mem);
			return;
		}

		size_t poolIndex = GetPoolIndexFromSize(maxNumBytes);

		// undo alignment
		uintptr_t origMem = reinterpret_cast<uintptr_t>(mem);
		uint8_t diff = *(reinterpret_cast<uint8_t*>(origMem - 1));
		origMem = diff > 0 ? origMem - diff : origMem - 256;

		// set "mem"s next pointer to be current head of the linked list
		if (m_currHead[poolIndex])
			memcpy(reinterpret_cast<void*>(origMem), &m_currHead[poolIndex], sizeof(void*));
		else
			memset(reinterpret_cast<void*>(origMem), 0, sizeof(void*));

		// update the head of linked list to point to "pMem"
		m_currHead[poolIndex] = reinterpret_cast<void*>(origMem);
	}
}

void* MemoryPool::AllocateNewBlock(size_t chunkSize) noexcept
{
	// allocate a new block of memory
	void* block = malloc(BLOCK_SIZE);

	// make this block a linked list i.e. in each chunk store a pointer to the next chunk
	uintptr_t currHead = reinterpret_cast<uintptr_t>(block);
	uintptr_t nextHead = currHead + chunkSize;
	const uintptr_t end = currHead + BLOCK_SIZE;

	while (nextHead < end)
	{
		memcpy(reinterpret_cast<void*>(currHead), &nextHead, sizeof(void*));
		currHead = nextHead;
		nextHead += chunkSize;
	}

	Assert(currHead == end - chunkSize, "bug");

	// make sure the last one points to null, so we'd know when to add a new block when this one becomes full
	memset(reinterpret_cast<void*>(currHead), 0, sizeof(void*));

	return block;
}

void MemoryPool::Grow(size_t poolIndex) noexcept
{
	size_t chunkSize = GetChunkSizeFromPoolIndex(poolIndex);

	// array of pointers to heads of memory blocks for this pool size. size has changed so we need the destroy the old one
	// and allocate a new one. moreover all the head pointers have to be copied over to this new array
	void** newMemoryBlockArray = (void**)malloc((m_numMemoryBlocks[poolIndex] + 1) * sizeof(void*));	
	Assert(newMemoryBlockArray, "malloc() failed.");

	void* newBlock = AllocateNewBlock(chunkSize);

	// copy over the pointers to previous memory blocks to this new array
	if (m_numMemoryBlocks[poolIndex])
	{
		memcpy(newMemoryBlockArray, m_pools[poolIndex], sizeof(void*) * m_numMemoryBlocks[poolIndex]);
	
		// free the old array
		Assert(m_pools[poolIndex], "this shouldn't be NULL");
		free(m_pools[poolIndex]);
	}

	Assert(m_currHead[poolIndex] == nullptr, "bug");

	// save the head pointer to this newly added memory block
	newMemoryBlockArray[m_numMemoryBlocks[poolIndex]] = newBlock;

	m_currHead[poolIndex] = newBlock;
	m_pools[poolIndex] = newMemoryBlockArray;
	m_numMemoryBlocks[poolIndex]++;
}

size_t MemoryPool::TotalSize() const
{
	size_t size = 0;

	for (int i = 0; i < POOL_COUNT; i++)
	{
		//size += GetChunkSizeFromPoolIndex(i) * NUM_CHUNKS * (m_numMemoryBlocks[i] + 1);
		size += BLOCK_SIZE * (m_numMemoryBlocks[i] + 1);
	}

	return size;
}

/*
void MemoryPool::Print()
{
	g_pApp->GetLogger().Log(ToString("Pool Count: ", m_poolCount));
	g_pApp->GetLogger().Log(ToString("Number of Chunks: ", m_numChunks));
	g_pApp->GetLogger().Log(ToString("Header Size: ", CHUNK_HEADER_SIZE));

	uint32_t runninSum = 0;
	for (int i = 0; i < m_poolCount; i++)
	{
		g_pApp->GetLogger().Log(ToString("\n************************************************\n",
			"Pool_", i, " Chunk Size: ", GetChunkSizeFromPoolIndex(i), " bytes"));

		uint32_t sum = static_cast<uint32_t>((GetChunkSizeFromPoolIndex(i) + CHUNK_HEADER_SIZE) * m_numChunks);
		runninSum += sum;

		g_pApp->GetLogger().Log(ToString("Pool_", i, " Total Size: ", sum, " bytes\n",
			"************************************************"));
	}

	g_pApp->GetLogger().Log(ToString("Total Size: ", runninSum, " bytes"));


		unsigned char *curr = m_memory + CHUNK_HEADER_SIZE;

		for (int e = 0; e < m_numElem; e++)
		{
			int64_t *i = (int64_t *)curr;
			std::cout << *i << "\n";
			curr += (sizeof(int64_t) + CHUNK_HEADER_SIZE);
		}
}
*/