#include "MemoryArena.h"

using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// StaticMemoryArena
//--------------------------------------------------------------------------------------

StaticMemoryArena::StaticMemoryArena(size_t s) noexcept
	: m_size(s),
	m_offset(0)
{
	m_mem = malloc(s);
}

StaticMemoryArena::~StaticMemoryArena() noexcept
{
	if (m_mem)
		free(m_mem);
}

StaticMemoryArena::StaticMemoryArena(StaticMemoryArena&& rhs) noexcept
{
	m_mem = rhs.m_mem;
	m_size = rhs.m_size;
	m_offset = rhs.m_offset;

	rhs.m_mem = nullptr;
	rhs.m_size = 0;
	rhs.m_offset = 0;
}

StaticMemoryArena& StaticMemoryArena::operator=(StaticMemoryArena&& rhs) noexcept
{
	m_mem = rhs.m_mem;
	m_size = rhs.m_size;
	m_offset = rhs.m_offset;

	rhs.m_mem = nullptr;
	rhs.m_size = 0;
	rhs.m_offset = 0;

	return *this;
}

void* StaticMemoryArena::AllocateAligned(size_t size, const char* name, int alignment) noexcept
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
