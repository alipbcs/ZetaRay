#pragma once

#include "../Utility/SmallVector.h"

namespace ZetaRay::Support
{
	class StaticMemoryArena
	{
	public:
		explicit StaticMemoryArena(size_t s) noexcept;
		~StaticMemoryArena() noexcept;

		StaticMemoryArena(StaticMemoryArena&&) noexcept;
		StaticMemoryArena& operator=(StaticMemoryArena&&) noexcept;

		void* AllocateAligned(size_t size, const char* name, int alignment = alignof(std::max_align_t)) noexcept;
		void FreeAligned(void* pMem, size_t size, const char* name, int alignment = alignof(std::max_align_t)) noexcept {};
		size_t TotalSize() const { return m_size; }

	private:
		void* m_mem;
		size_t m_size;
		size_t m_offset;
	};

	class DynamicMemoryArena
	{
	public:
		explicit DynamicMemoryArena(size_t blockSize) noexcept;
		~DynamicMemoryArena() noexcept;

		DynamicMemoryArena(DynamicMemoryArena&&) noexcept;
		DynamicMemoryArena& operator=(DynamicMemoryArena&&) noexcept;

		void* AllocateAligned(size_t size, const char* name, int alignment = alignof(std::max_align_t)) noexcept;
		void FreeAligned(void* pMem, size_t size, const char* name, int alignment = alignof(std::max_align_t)) noexcept {};
		//size_t TotalSize() const { return m_size; }

	private:
		//Util::SmallVector<uint8_t*, 8> m_blocks;
	};

	struct StaticArenaAllocator
	{
		StaticArenaAllocator(StaticMemoryArena& ma) noexcept
			: m_allocator(&ma)
		{}

		StaticArenaAllocator(const StaticArenaAllocator& other) noexcept
			: m_allocator(other.m_allocator)
		{
		}

		StaticArenaAllocator& operator=(const StaticArenaAllocator& other) noexcept
		{
			m_allocator = other.m_allocator;
			return *this;
		}

		__forceinline void* AllocateAligned(size_t size, const char* name, int alignment) noexcept
		{
			return m_allocator->AllocateAligned(size, name, alignment);
		}

		__forceinline void FreeAligned(void* mem, size_t size, const char* name, int alignment) noexcept
		{
			m_allocator->FreeAligned(mem, size, name, alignment);
		}

	private:
		StaticMemoryArena* m_allocator;
	};
}
