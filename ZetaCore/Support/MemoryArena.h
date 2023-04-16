#pragma once

#include "../Utility/SmallVector.h"

namespace ZetaRay::Support
{
	class MemoryArena
	{
	public:
		explicit MemoryArena(size_t blockSize = 64 * 1024) noexcept;
		~MemoryArena() noexcept = default;

		MemoryArena(MemoryArena&&) noexcept;
		MemoryArena& operator=(MemoryArena&&) noexcept;

		void* AllocateAligned(size_t size, size_t alignment = alignof(std::max_align_t)) noexcept;
		void FreeAligned(void* pMem, size_t size, size_t alignment = alignof(std::max_align_t)) noexcept {};
		size_t TotalSize() const;
		void Reset() noexcept;

	private:
		struct MemoryBlock
		{
			MemoryBlock() noexcept = default;
			explicit MemoryBlock(size_t size) noexcept
			{
				Start = malloc(size);
				Offset = 0;
				Size = size;
			}
			~MemoryBlock() noexcept
			{
				if (Start)
					free(Start);

				Start = nullptr;
				Offset = 0;
				Size = 0;
			}

			MemoryBlock(MemoryBlock&& rhs) noexcept
				: Start(rhs.Start),
				Offset(rhs.Offset),
				Size(rhs.Size)
			{
				rhs.Start = nullptr;
				rhs.Offset = 0;
				rhs.Size = 0;
			}

			MemoryBlock& operator=(MemoryBlock&& rhs) noexcept
			{
				Start = rhs.Start;
				Offset = rhs.Offset;
				Size = rhs.Size;

				rhs.Start = nullptr;
				rhs.Offset = 0;
				rhs.Size = 0;

				return *this;
			}

			void* Start;
			uintptr_t Offset;
			size_t Size;
		};

		const size_t m_blockSize;
		Util::SmallVector<MemoryBlock, SystemAllocator, 8> m_blocks;

#ifdef _DEBUG
		uint32_t m_numAllocs = 0;
#endif // _DEBUG
	};

	struct ArenaAllocator
	{
		ArenaAllocator(MemoryArena& ma) noexcept
			: m_allocator(&ma)
		{}

		ArenaAllocator(const ArenaAllocator& other) noexcept
			: m_allocator(other.m_allocator)
		{
		}

		ArenaAllocator& operator=(const ArenaAllocator& other) noexcept
		{
			m_allocator = other.m_allocator;
			return *this;
		}

		ZetaInline void* AllocateAligned(size_t size, size_t alignment = alignof(std::max_align_t)) noexcept
		{
			return m_allocator->AllocateAligned(size, alignment);
		}

		ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment = alignof(std::max_align_t)) noexcept
		{
			m_allocator->FreeAligned(mem, size, alignment);
		}

	private:
		MemoryArena* m_allocator;
	};
}
