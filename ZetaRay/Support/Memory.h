#pragma once

#include "../Core/ZetaRay.h"
#include <concepts>

namespace ZetaRay::Support
{
	template<typename T>
	concept AllocType = requires(T t, size_t s, const char* n, int a, void* mem)
	{
		{ t.AllocateAligned(s, n, a) } -> std::same_as<void*>;
		{ t.FreeAligned(mem, s, n, a) } -> std::same_as<void>;
	};

	struct SystemAllocator
	{
		__forceinline void* AllocateAligned(size_t size, const char* name, int alignment) noexcept
		{
			return _aligned_malloc(size, alignment);
		}

		__forceinline void FreeAligned(void* mem, size_t size, const char* name, int alignment) noexcept
		{
			_aligned_free(mem);
		}
	};
}
