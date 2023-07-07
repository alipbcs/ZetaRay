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
		Span(T(&arr)[N]) noexcept
			: m_ptr(arr),
			m_size(N)
		{}

		Span(const char* str) noexcept
			: m_ptr(str),
			m_size(strlen(str))
		{
		}

		ZetaInline bool empty() const noexcept
		{
			return m_ptr == nullptr || m_size == 0;;
		}

		ZetaInline T* begin() noexcept
		{
			return m_ptr;
		}

		ZetaInline T* end() noexcept
		{
			return m_ptr + m_size;
		}

		ZetaInline const T* begin() const noexcept
		{
			return m_ptr;
		}

		ZetaInline const T* end() const noexcept
		{
			return m_ptr + m_size;
		}

		ZetaInline T& operator[](size_t pos) noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		ZetaInline const T& operator[](size_t pos) const noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		ZetaInline size_t size() const noexcept
		{
			return m_size;
		}

		ZetaInline T* data() noexcept
		{
			return m_ptr;
		}

	private:
		T* m_ptr;
		size_t m_size;
	};


	// Span for strings
	// Note that underlying string is not necessarily null-terminated.
	struct StrView
	{
		StrView(const char* ptr, size_t n) noexcept
			: m_ptr(ptr),
			m_size(n)
		{}

		StrView(const char* str) noexcept
			: m_ptr(str),
			m_size(strlen(str))
		{
		}

		ZetaInline bool empty() noexcept
		{
			return m_ptr == nullptr;
		}

		ZetaInline const char* begin() const noexcept
		{
			return m_ptr;
		}

		ZetaInline const char* end() const noexcept
		{
			return m_ptr + m_size;
		}

		ZetaInline const char& operator[](size_t pos) const noexcept
		{
			Assert(pos < m_size, "Out-of-bound access.");
			return *(m_ptr + pos);
		}

		// Note that terminating null character (if any) is not counted
		ZetaInline size_t size() const noexcept
		{
			return m_size;
		}

		ZetaInline const char* data() noexcept
		{
			return m_ptr;
		}

	private:
		const char* m_ptr;
		size_t m_size;
	};
}

