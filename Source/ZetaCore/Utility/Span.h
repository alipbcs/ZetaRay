#pragma once

#include "SmallVector.h"

namespace ZetaRay::Util
{
    // Doesn't own the data -- just a pointer and a size.
    template<typename T>
    struct MutableSpan
    {
        MutableSpan(T* ptr, size_t n)
            : m_ptr(ptr),
            m_size(n)
        {}

        template<typename Allocator = Support::SystemAllocator, uint32_t N>
        MutableSpan(SmallVector<T, Allocator, N>& vec)
            : m_ptr(vec.data()),
            m_size(vec.size())
        {}

        template<typename Allocator = Support::SystemAllocator>
        MutableSpan(Vector<T, Allocator>& vec)
            : m_ptr(vec.data()),
            m_size(vec.size())
        {}

        template<size_t N>
        MutableSpan(T(&arr)[N])
            : m_ptr(arr),
            m_size(N)
        {}

        ZetaInline bool empty() const
        {
            return m_ptr == nullptr || m_size == 0;
        }

        ZetaInline T* begin() const
        {
            return m_ptr;
        }

        ZetaInline T* end() const
        {
            return m_ptr + m_size;
        }

        ZetaInline T& operator[](size_t pos) const
        {
            Assert(pos < m_size, "Out-of-bound access.");
            return *(m_ptr + pos);
        }

        ZetaInline size_t size() const
        {
            return m_size;
        }

        ZetaInline T* data() const
        {
            return m_ptr;
        }

    private:
        T* m_ptr;
        size_t m_size;
    };

    // Doesn't own the data -- just a pointer and a size. Read-only access.
    template<typename T>
    struct Span
    {
        Span(const T* ptr, size_t n)
            : m_ptr(ptr),
            m_size(n)
        {}

        template<typename Allocator = Support::SystemAllocator, uint32_t N>
        Span(const SmallVector<T, Allocator, N>& vec)
            : m_ptr(vec.data()),
            m_size(vec.size())
        {}

        template<typename Allocator = Support::SystemAllocator>
        Span(const Vector<T, Allocator>& vec)
            : m_ptr(vec.data()),
            m_size(vec.size())
        {}

        template<size_t N>
        Span(T(&arr)[N])
            : m_ptr(static_cast<const T*>(arr)),
            m_size(N)
        {}

        Span(MutableSpan<T> span)
            : m_ptr(static_cast<const T*>(span.data())),
            m_size(span.size())
        {}

        ZetaInline bool empty() const
        {
            return m_ptr == nullptr || m_size == 0;;
        }

        ZetaInline const T* begin() const
        {
            return m_ptr;
        }

        ZetaInline const T* end() const
        {
            return m_ptr + m_size;
        }

        ZetaInline const T& operator[](size_t pos) const
        {
            Assert(pos < m_size, "Out-of-bound access.");
            return *(m_ptr + pos);
        }

        ZetaInline size_t size() const
        {
            return m_size;
        }

        ZetaInline const T* data() const
        {
            return m_ptr;
        }

    private:
        const T* m_ptr;
        size_t m_size;
    };

    // Span for strings
    // Note that the underlying string is not necessarily null-terminated.
    struct StrView
    {
        StrView(const char* str, size_t n)
            : m_ptr(str),
            m_size(n)
        {}

        // Assumes string is null-terminated.
        StrView(const char* str)
            : m_ptr(str),
            m_size(strlen(str))
        {}

        ZetaInline bool empty() const
        {
            return m_ptr == nullptr;
        }

        ZetaInline const char* begin() const
        {
            return m_ptr;
        }

        ZetaInline const char* end() const
        {
            return m_ptr + m_size;
        }

        ZetaInline const char& operator[](size_t pos) const
        {
            Assert(pos < m_size, "Out-of-bound access.");
            return *(m_ptr + pos);
        }

        // Note that the terminating null character (if any) is not counted
        ZetaInline size_t size() const
        {
            return m_size;
        }

        ZetaInline const char* data() const
        {
            return m_ptr;
        }

    private:
        const char* m_ptr;
        size_t m_size;
    };
}
