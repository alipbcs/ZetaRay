#pragma once

#include "../Win32/App.h"
#include <memory>

namespace ZetaRay
{
	template <typename T, int Alignment = alignof(T)>
	class StlMemoryAllocator
	{
	public:
		using value_type = T;

		StlMemoryAllocator() noexcept
		{}

		template <typename U> StlMemoryAllocator(const StlMemoryAllocator<U, Alignment>&) noexcept
		{}

		template <typename U> 
		struct rebind 
		{ 
			typedef StlMemoryAllocator<U, Alignment> other;
		};

		[[nodiscard]] T* allocate(size_t n) noexcept
		{
			MemoryPool& pool = g_pApp->GetAllocator();
			T* mem = reinterpret_cast<T*>(pool.AllocateAligned(n * sizeof(T), Alignment));
			//assert((((uintptr_t)mem) & (Alignment - 1)) == 0 && "Allocated memory had incorrect alignment.");

			return mem;
		}

		void deallocate(T* p, size_t n) noexcept
		{
			MemoryPool& pool = g_pApp->GetAllocator();
			pool.FreeAligned(reinterpret_cast<void*>(p), n * sizeof(T), Alignment);

			//free(p);
		}
	};

	template <typename T, int A1, typename U, int A2>
	constexpr bool operator == (const StlMemoryAllocator<T, A1>&, const StlMemoryAllocator<U, A2>&) noexcept
	{
		return A1 == A2;
	}

	template <typename T, int A1, typename U, int A2>
	constexpr bool operator != (const StlMemoryAllocator<T, A1>& a, const StlMemoryAllocator<U, A2>& b) noexcept
	{
		return A1 != A2;
	}

	template<typename T>
	struct CustomDeleter
	{
		void operator()(T* p) noexcept
		{
			StlMemoryAllocator<T> allocator;
			//std::allocator_traits<MemoryAllocator<T>>::deallocate(a, p, 1);
			allocator.deallocate(p, 1);
		}
	};

	template<typename T, typename... Args>
	inline std::unique_ptr<T, CustomDeleter<T>> AllocateUnique(Args&&... args) noexcept
	{
		StlMemoryAllocator<T> allocator = StlMemoryAllocator<T>();
		T* pp = allocator.allocate(1);
		new (pp) T(ZetaForward(args)...);

		return std::unique_ptr<T, CustomDeleter<T>>(pp, CustomDeleter<T>());
	}
}