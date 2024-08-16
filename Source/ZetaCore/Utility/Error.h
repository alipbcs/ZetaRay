#pragma once

#include "../App/ZetaRay.h"
#include <stb/stb_sprintf.h>

//--------------------------------------------------------------------------------------
// Stack-allocated string with a maximum size
//--------------------------------------------------------------------------------------

// Note: stbsp_snprintf always returns a zero-terminated string.

#ifndef NDEBUG
#define StackStr(buffName, lenName, formatStr, ...)                         \
    char buffName[512];                                                     \
    int lenName = stbsp_snprintf(buffName, 512, formatStr, __VA_ARGS__);    
#else
#define StackStr(buffName, lenName, formatStr, ...)                         \
    char buffName[512];                                                     \
    int lenName = stbsp_snprintf(buffName, 512, formatStr, __VA_ARGS__);    
#endif

//--------------------------------------------------------------------------------------
// Error checking
//--------------------------------------------------------------------------------------

namespace ZetaRay::Util
{
    void ReportError(const char* title, const char* msg);
    void ReportErrorWin32(const char* file, int line, const char* call);
    void DebugBreak();
    void Exit();
}

#ifndef NDEBUG 
#define Assert(expr, formatStr, ...)                                                          \
    if(!(expr))                                                                               \
    {                                                                                         \
        char buff_[256];                                                                      \
        int n_ = stbsp_snprintf(buff_, 256, "%s: %d\n", __FILE__, __LINE__);                  \
        stbsp_snprintf(buff_ + n_, 256 - n_, formatStr, __VA_ARGS__);                         \
        ZetaRay::Util::ReportError("Assertion failed", buff_);                                \
        ZetaRay::Util::DebugBreak();                                                          \
    }
#else
#define Assert(expr, formatStr, ...) ((void)0)
#endif

#ifndef NDEBUG 
#define CheckWin32(expr)                                            \
    [[unlikely]]                                                    \
    if(!(expr))                                                     \
    {                                                               \
        ZetaRay::Util::ReportErrorWin32(__FILE__, __LINE__, #expr); \
        ZetaRay::Util::DebugBreak();                                \
    }
#else
#define CheckWin32(expr)                                            \
    [[unlikely]]                                                    \
    if(!(expr))                                                     \
    {                                                               \
        ZetaRay::Util::ReportErrorWin32(__FILE__, __LINE__, #expr); \
        ZetaRay::Util::Exit();                                      \
    }
#endif

#ifndef NDEBUG
#ifndef Check
#define Check(expr, formatStr, ...)                                                          \
    [[unlikely]]                                                                             \
    if(!(expr))                                                                              \
    {                                                                                        \
        char buff_[256];                                                                     \
        int n_ = stbsp_snprintf(buff_, 256, "%s: %d\n", __FILE__, __LINE__);                 \
        stbsp_snprintf(buff_ + n_, 256 - n_, formatStr, __VA_ARGS__);                        \
        ZetaRay::Util::ReportError("Fatal Error", buff_);                                    \
        ZetaRay::Util::DebugBreak();                                                         \
    }
#endif
#else
#ifndef Check
#define Check(expr, formatStr, ...)                                                          \
    [[unlikely]]                                                                             \
    if(!(expr))                                                                              \
    {                                                                                        \
        char buff_[256];                                                                     \
        int n_ = stbsp_snprintf(buff_, 256, "%s: %d\n", __FILE__, __LINE__);                 \
        stbsp_snprintf(buff_ + n_, 256 - n_, formatStr, __VA_ARGS__);                        \
        ZetaRay::Util::ReportError("Fatal Error", buff_);                                    \
        ZetaRay::Util::Exit();                                                               \
    }
#endif
#endif
