#pragma once

#include "../Win32/Win32.h"
#include "Span.h"

namespace ZetaRay::Util
{
    template<typename T>
    struct RSynchronizedView
    {
        RSynchronizedView(const T& t, SRWLOCK& lock)
            : m_view(t),
            m_lock(lock)
        {
            AcquireSRWLockShared(&m_lock);
        }
        ~RSynchronizedView()
        {
            ReleaseSRWLockShared(&m_lock);
        }
        RSynchronizedView(RSynchronizedView&&) = delete;
        RSynchronizedView& operator=(RSynchronizedView&&) = delete;

        const T& View() const { return m_view; }

    private:
        const T& m_view;
        SRWLOCK& m_lock;
    };

    template<typename T>
    struct RWSynchronizedView
    {
        RWSynchronizedView(T& t, SRWLOCK& lock)
            : m_view(t),
            m_lock(lock)
        {
            AcquireSRWLockExclusive(&m_lock);
        }
        ~RWSynchronizedView()
        {
            ReleaseSRWLockExclusive(&m_lock);
        }
        RWSynchronizedView(RWSynchronizedView&&) = delete;
        RWSynchronizedView& operator=(RWSynchronizedView&&) = delete;

        T& View() const { return m_view; }

    private:
        T& m_view;
        SRWLOCK& m_lock;
    };

    template<typename T>
    struct SynchronizedSpan
    {
        SynchronizedSpan(Util::Span<T> t, SRWLOCK& lock)
            : m_span(t),
            m_lock(lock)
        {
            AcquireSRWLockShared(&m_lock);
        }
        ~SynchronizedSpan()
        {
            ReleaseSRWLockShared(&m_lock);
        }
        SynchronizedSpan(SynchronizedSpan&&) = delete;
        SynchronizedSpan& operator=(SynchronizedSpan&&) = delete;

        const Util::Span<T> m_span;
    private:
        SRWLOCK& m_lock;
    };    
    
    template<typename T>
    struct SynchronizedMutableSpan
    {
        SynchronizedMutableSpan(Util::MutableSpan<T> t, SRWLOCK& lock)
            : m_span(t),
            m_lock(lock)
        {
            AcquireSRWLockExclusive(&m_lock);
        }
        ~SynchronizedMutableSpan()
        {
            ReleaseSRWLockExclusive(&m_lock);
        }
        SynchronizedMutableSpan(SynchronizedMutableSpan&&) = delete;
        SynchronizedMutableSpan& operator=(SynchronizedMutableSpan&&) = delete;

        const Util::MutableSpan<T> m_span;
    private:
        SRWLOCK& m_lock;
    };
}