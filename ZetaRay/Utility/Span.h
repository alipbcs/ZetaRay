#pragma once

#include "SmallVector.h"

namespace ZetaRay::Util
{
	template<typename T>
	struct Span
	{
		Span(T* ptr, size_t n) noexcept
			: m_ptr(ptr),
			m_size(n)
		{}

		template<typename Allocator = Support::SystemAllocator, uint32_t N>
		Span(SmallVector<T, Allocator, N>& vec) noexcept
			: m_ptr(vec.data()),
			m_size(vec.size())
		{}

		template<typename Allocator = Support::SystemAllocator>
		Span(Vector<T, Allocator>& vec) noexcept
			: m_ptr(vec.data()),
			m_size(vec.size())
		{}

		template<size_t N>
		Span(T(&arr)[N])
			: m_ptr(arr),
			m_size(N)
		{}

		__forceinline T* begin() noexcept
		{
			return m_ptr;
		}

		__forceinline T* end() noexcept
		{
			return m_ptr + m_size;
		}

		__forceinline const T* begin() const noexcept
		{
			return m_ptr;
		}

		__forceinline const T* end() const noexcept
		{
			return m_ptr + m_size;
		}

		__forceinline T& operator[](size_t pos) noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		__forceinline const T& operator[](size_t pos) const noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		size_t size() const noexcept
		{
			return m_size;
		}

		T* data() noexcept
		{
			return m_ptr;
		}

	private:
		T* m_ptr;
		size_t m_size;
	};
}