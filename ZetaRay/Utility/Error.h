#pragma once

#include "../Core/ZetaRay.h"
#include <stb/stb_sprintf.h>

//--------------------------------------------------------------------------------------
// Stack-allocated string with a maximum size of 256
//--------------------------------------------------------------------------------------

#ifdef _DEBUG
#define StackStr(buffName, lenName, formatStr, ...)                         \
    char buffName[256];                                                     \
    int lenName = stbsp_snprintf(buffName, 256, formatStr, __VA_ARGS__);    
#else
#define StackStr(buffName, lenName, formatStr, ...)                         \
    char buffName[256];                                                     \
    int lenName = stbsp_snprintf(buffName, 256, formatStr, __VA_ARGS__);    
#endif

//--------------------------------------------------------------------------------------
// Error checking
//--------------------------------------------------------------------------------------

namespace ZetaRay::Util
{
    void ReportError(const char* title, const char* msg) noexcept;
    void ReportErrorWin32(const char* file, int line, const char* call) noexcept;
    void DebugBreak() noexcept;
    void Exit() noexcept;
}

#ifdef _DEBUG
#define Assert(expr, formatStr, ...)                                                          \
    if(!(expr))                                                                               \
    {                                                                                         \
        char buff[256];                                                                       \
        int n = stbsp_snprintf(buff, 256, "%s: %d\n", __FILE__, __LINE__);                    \
        stbsp_snprintf(buff + n, 256 - n, formatStr, __VA_ARGS__);                            \
        ZetaRay::Util::ReportError("Assertion failed", buff);                                 \
        ZetaRay::Util::DebugBreak();                                                          \
    }
#else
#define Assert(expr, formatStr, ...) ((void)0)
#endif

#ifdef _DEBUG
#define CheckWin32(expr)                                            \
    if(!(expr))                                                     \
    {                                                               \
        ZetaRay::Util::ReportErrorWin32(__FILE__, __LINE__, #expr); \
        ZetaRay::Util::DebugBreak();                                \
    }
#else
#define CheckWin32(expr)                                            \
    if(!(expr))                                                     \
    {                                                               \
        ZetaRay::Util::ReportErrorWin32(__FILE__, __LINE__, #expr); \
        ZetaRay::Util::Exit();                                      \
    }
#endif

#ifdef _DEBUG   
#ifndef Check
#define Check(expr, formatStr, ...)                                                          \
    if(!(expr))                                                                              \
    {                                                                                        \
        char buff[256];                                                                      \
        int n = stbsp_snprintf(buff, 256, "%s: %d\n", __FILE__, __LINE__);                   \
        stbsp_snprintf(buff + n, 256 - n, formatStr, __VA_ARGS__);                           \
        ZetaRay::Util::ReportError("Fatal Error", buff);                                     \
        ZetaRay::Util::DebugBreak();                                                         \
    }
#endif
#else
#ifndef Check
#define Check(expr, formatStr, ...)                                                          \
    if(!(expr))                                                                              \
    {                                                                                        \
        char buff[256];                                                                      \
        int n = stbsp_snprintf(buff, 256, "%s: %d\n", __FILE__, __LINE__);                   \
        stbsp_snprintf(buff + n, 256 - n, formatStr, __VA_ARGS__);                           \
        ZetaRay::Util::ReportError("Fatal Error", buff);                                     \
        ZetaRay::Util::Exit();                                                               \
    }
#endif
#endif

