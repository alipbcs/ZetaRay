#pragma once

#include "../Win32/Win32.h"

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
	struct RSynchronizedVariable
	{
		static_assert(std::is_copy_assignable_v<T>, "T must be copy assignable.");

		RSynchronizedVariable(T t, SRWLOCK& lock)
			: m_var(t),
			m_lock(lock)
		{
			AcquireSRWLockShared(&m_lock);
		}

		~RSynchronizedVariable()
		{
			ReleaseSRWLockShared(&m_lock);
		}

		RSynchronizedVariable(RSynchronizedVariable&&) = delete;
		RSynchronizedVariable& operator=(RSynchronizedVariable&&) = delete;

		const T Variable() const { return m_var; }

	private:
		const T m_var;
		SRWLOCK& m_lock;
	};

	template<typename T>
	struct RWSynchronizedVariable
	{
		static_assert(std::is_copy_assignable_v<T>, "T must be copy assignable.");

		RWSynchronizedVariable(T t, SRWLOCK& lock)
			: m_var(t),
			m_lock(lock)
		{
			AcquireSRWLockExclusive(&m_lock);
		}

		~RWSynchronizedVariable()
		{
			ReleaseSRWLockExclusive(&m_lock);
		}

		RWSynchronizedVariable(RWSynchronizedVariable&&) = delete;
		RWSynchronizedVariable& operator=(RWSynchronizedVariable&&) = delete;

		T Variable() const { return m_var; }

	private:
		T m_var;
		SRWLOCK& m_lock;
	};
}