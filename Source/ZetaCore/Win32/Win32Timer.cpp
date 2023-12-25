#pragma once

#include "../Utility/Error.h"
#include "../App/Timer.h"
#include "Win32.h"

using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// Timer
//--------------------------------------------------------------------------------------

Timer::Timer()
{
    LARGE_INTEGER freq;
    bool success = QueryPerformanceFrequency(&freq);
    Assert(success, "QueryPerformanceFrequency() failed.");
    m_counterFreqSec = freq.QuadPart;
}

void Timer::Start()
{
    LARGE_INTEGER currCount;
    CheckWin32(QueryPerformanceCounter(&currCount));
    m_start = currCount.QuadPart;
}

void Timer::Resume()
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

void Timer::Pause()
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

void Timer::Tick()
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

    // there are "m_counterFreqSec" counts per second. by keeping track
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

//--------------------------------------------------------------------------------------
// DeltaTimer
//--------------------------------------------------------------------------------------

DeltaTimer::DeltaTimer()
{
    LARGE_INTEGER freq;
    bool success = QueryPerformanceFrequency(&freq);
    Assert(success, "QueryPerformanceFrequency() failed.");
    m_counterFreqSec = freq.QuadPart;
}

void DeltaTimer::Start()
{
    LARGE_INTEGER currCount;
    CheckWin32(QueryPerformanceCounter(&currCount));
    m_start = currCount.QuadPart;
}

void DeltaTimer::End()
{
    LARGE_INTEGER currCount;
    CheckWin32(QueryPerformanceCounter(&currCount));
    m_end = currCount.QuadPart;
}

double DeltaTimer::DeltaNano()
{
    // convert to microseconds before dividing to avoid precision loss
    // https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
    int64_t elapsedMicro = 1'000'000'000 * (m_end - m_start);
    return (double)elapsedMicro / m_counterFreqSec;
}

double DeltaTimer::DeltaMicro()
{
    // convert to microseconds before dividing to avoid precision loss
    // https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
    int64_t elapsedMicro = 1'000'000 * (m_end - m_start);
    return (double)elapsedMicro / m_counterFreqSec;
}

double DeltaTimer::DeltaMilli()
{
    // convert to milliseconds before dividing to avoid precision loss
    // https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
    int64_t elapsedMicro = 1'000 * (m_end - m_start);
    return (double)elapsedMicro / m_counterFreqSec;
}
