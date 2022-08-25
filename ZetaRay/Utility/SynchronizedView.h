#pragma once

#include "../Win32/Win32.h"

namespace ZetaRay::Util
{
	template<typename T>
	struct RSynchronizedView
	{
		RSynchronizedView(const T& t, SRWLOCK& lock) noexcept
			: m_view(t),
			m_lock(lock)
		{
			AcquireSRWLockShared(&m_lock);
		}

		~RSynchronizedView() noexcept
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
		RWSynchronizedView(T& t, SRWLOCK& lock) noexcept
			: m_view(t),
			m_lock(lock)
		{
			AcquireSRWLockExclusive(&m_lock);
		}

		~RWSynchronizedView() noexcept
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
}