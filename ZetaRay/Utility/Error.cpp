#include "Error.h"
#include "../Win32/Win32.h"

using namespace ZetaRay::Util;

void ZetaRay::Util::ReportError(const char* title, const char* msg) noexcept
{
    MessageBoxA(nullptr, msg, title, MB_ICONERROR | MB_OK);
}

void ZetaRay::Util::ReportErrorWin32(const char* file, int line, const char* call) noexcept
{
    char buff[256];
    stbsp_snprintf(buff, 256, "%s: %d\nWin32 call %s failed with following error code: %d", file, line, call, GetLastError());
    MessageBoxA(nullptr, buff, "Assertion failed", MB_ICONERROR | MB_OK);
}

void ZetaRay::Util::DebugBreak() noexcept
{
	__debugbreak();
}

void ZetaRay::Util::Exit() noexcept
{
    exit(EXIT_FAILURE);
}
