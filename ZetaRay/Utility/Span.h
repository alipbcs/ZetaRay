#pragma once

#include "SmallVector.h"

namespace ZetaRay
{
	template<typename T>
	struct Span
	{
		Span(T* ptr, size_t n) noexcept
			: m_ptr(ptr),
			m_size(n)
		{}

		template<uint32_t N>
		Span(SmallVector<T, N>& vec) noexcept
			: m_ptr(vec.data()),
			m_size(vec.size())
		{}

		Span(Vector<T>& vec) noexcept
			: m_ptr(vec.data()),
			m_size(vec.size())
		{}

		template<size_t N>
		Span(T(&arr)[N])
			: m_ptr(arr),
			m_size(N)
		{}

		inline T* begin() noexcept
		{
			return m_ptr;
		}

		inline T* end() noexcept
		{
			return m_ptr + m_size;
		}

		inline const T* begin() const noexcept
		{
			return m_ptr;
		}

		inline const T* end() const noexcept
		{
			return m_ptr + m_size;
		}

		inline T& operator[](size_t pos) noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		inline const T& operator[](size_t pos) const noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		inline size_t size() const noexcept
		{
			return m_size;
		}

		inline T* data() noexcept
		{
			return m_ptr;
		}

	private:
		T* m_ptr;
		size_t m_size;
	};
}