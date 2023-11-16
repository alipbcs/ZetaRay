#pragma once

#include "../Utility/Error.h"

namespace ZetaRay::App
{
	struct Timer
	{
		Timer();
		~Timer() = default;

		// Returns elapsed time since the last time Tick() was called
		ZetaInline double GetElapsedTime() const { return m_delta; }

		// Get total number of updates since start of the program
		ZetaInline uint64_t GetTotalFrameCount() const { return m_frameCount; }
		ZetaInline int GetFramesPerSecond() const { return m_fps; }
		ZetaInline int64_t GetCounterFreq() const { return m_counterFreqSec; }

		void Start();
		void Resume();
		void Pause();
		void Tick();

		// Get total time since the start of the program.
		ZetaInline double GetTotalTime() const
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
		//double m_totalTime = 0.0;

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
		DeltaTimer();

		void Start();
		void End();
		double DeltaMicro();
		double DeltaMilli();
		double DeltaNano();

	private:
		// frequency of the counter. Units are counts/sec
		int64_t m_counterFreqSec;
		int64_t m_start = 0;
		int64_t m_end = 0;
	};
}
