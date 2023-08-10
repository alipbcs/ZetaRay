#pragma once

#include "../App/ZetaRay.h"
#include <malloc.h>
#include <concepts>

namespace ZetaRay::Support
{
	template<typename T>
	concept AllocType = requires(T t, size_t s, size_t a, void* mem)
	{
		{ t.AllocateAligned(s, a) } -> std::same_as<void*>;
		{ t.FreeAligned(mem, s, a) } -> std::same_as<void>;
	};

	struct SystemAllocator
	{
		ZetaInline void* AllocateAligned(size_t size, size_t alignment) noexcept
		{
			return _aligned_malloc(size, alignment);
		}

		ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment) noexcept
		{
			_aligned_free(mem);
		}
	};
}
