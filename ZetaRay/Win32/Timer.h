#pragma once

#include "../Utility/Error.h"
#include "Win32.h"

namespace ZetaRay::Win32
{
	struct Timer
	{
		Timer() noexcept
		{
			LARGE_INTEGER freq;
			bool success = QueryPerformanceFrequency(&freq);
			Assert(success, "QueryPerformanceFrequency() failed.");
			m_counterFreqSec = freq.QuadPart;
		}

		~Timer() noexcept
		{}

		// Returns elapsed time since the last time Tick() was called
		double GetElapsedTime() const { return m_delta; }

		// Get total number of updates since start of the program
		uint64_t GetTotalFrameCount() const { return m_frameCount; }
		int GetFramesPerSecond() const { return m_fps; }
		int64_t GetCounterFreq() const { return m_counterFreqSec; }

		void Start() noexcept
		{
			LARGE_INTEGER currCount;
			CheckWin32(QueryPerformanceCounter(&currCount));
			m_start = currCount.QuadPart;
		}

		void Resume() noexcept
		{
			LARGE_INTEGER currCount;
			CheckWin32(QueryPerformanceCounter(&currCount));
			m_last = currCount.QuadPart;

			if (m_paused)
			{
				m_totalPausedCounts += m_last - m_pauseCount;
				m_pauseCount = 0;

				m_paused = false;
			}
		}

		void Pause() noexcept
		{
			if (m_paused)
				return;

			LARGE_INTEGER currCount;
			CheckWin32(QueryPerformanceCounter(&currCount));
			m_pauseCount = currCount.QuadPart;

			m_framesInLastSecond = 0;
			m_numCountsInLastSecond = 0;
			m_paused = true;
		}

		void Tick() noexcept
		{
			if (m_paused)
				return;

			LARGE_INTEGER currCount;
			CheckWin32(QueryPerformanceCounter(&currCount));

			m_elapsedCounts = currCount.QuadPart - m_last;
			m_numCountsInLastSecond += m_elapsedCounts;
			m_framesInLastSecond++;
			m_last = currCount.QuadPart;

			m_delta = (double)m_elapsedCounts / m_counterFreqSec;
			m_counterFreqSec += (int64_t)m_delta;

			// there are "m_counterFreqSec" counts per second. by keeping tracks
			// of number of counts we can know when one second has passed. number
			// of times Tick() was called during that one second is equal to FPS
			if (m_numCountsInLastSecond >= m_counterFreqSec)
			{
				m_fps = (int)m_framesInLastSecond;
				m_framesInLastSecond = 0;
				m_numCountsInLastSecond = 0;
			}

			m_frameCount++;
		}

		// Get total time since the start of the program.
		double GetTotalTime() const
		{
			int64_t numCounts;

			if (!m_paused)
				numCounts = ((m_last - m_start) - m_totalPausedCounts);
			else
				numCounts = ((m_pauseCount - m_start) - m_totalPausedCounts);

			return (double)numCounts / m_counterFreqSec;
		}

	private:
		// frequency of the counter. Units are counts/sec
		int64_t m_counterFreqSec;

		// last queried count
		int64_t m_last;

		// app start count
		int64_t m_start;

		// track duration of time when the app was stopped
		int64_t m_pauseCount = 0;
		int64_t m_totalPausedCounts = 0;
		bool m_paused = false;

		// number of frames rendered since the program started
		uint64_t m_frameCount = 0;

		// total number of counts since the program started
		double m_totalTime = 0.0;

		// frames per-second
		int64_t m_framesInLastSecond = 0;
		int64_t m_numCountsInLastSecond = 0;
		int m_fps = 0;

		// counts since the last update
		int64_t m_elapsedCounts;
		// time passed since the last update
		double m_delta;
	};


	struct DeltaTimer
	{
		DeltaTimer() noexcept
		{
			LARGE_INTEGER freq;
			bool success = QueryPerformanceFrequency(&freq);
			Assert(success, "QueryPerformanceFrequency() failed.");
			m_counterFreqSec = freq.QuadPart;
		}

		void Start() noexcept
		{
			LARGE_INTEGER currCount;
			CheckWin32(QueryPerformanceCounter(&currCount));
			m_start = currCount.QuadPart;
		}

		void End() noexcept
		{
			LARGE_INTEGER currCount;
			CheckWin32(QueryPerformanceCounter(&currCount));
			m_end = currCount.QuadPart;
		}

		double DeltaMicro() noexcept
		{
			// convert to microseconds before dividing to avoid precision loss
			// https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
			int64_t elapsedMicro = 1'000'000 * (m_end - m_start);
			return (double)elapsedMicro / m_counterFreqSec;
		}

		double DeltaMilli() noexcept
		{
			// convert to milliseconds before dividing to avoid precision loss
			// https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
			int64_t elapsedMicro = 1'000 * (m_end - m_start);
			return (double)elapsedMicro / m_counterFreqSec;
		}

	private:
		// frequency of the counter. Units are counts/sec
		int64_t m_counterFreqSec;
		int64_t m_start = 0;
		int64_t m_end = 0;
	};
}
