#pragma once

#include "App.h"
#include "../Utility/Error.h"

#ifdef _DEBUG
#define LOG_CONSOLE(formatStr, ...)          \
{                                            \
    ZetaRay::App::LockStdOut();              \
    printf(formatStr, __VA_ARGS__);          \
    ZetaRay::App::UnlockStdOut();            \
}
#else
#define LOG(formatStr, ...)        ((void)0)
#endif

#define LOG_UI(TYPE, formatStr, ...) LOG_UI_##TYPE(formatStr, __VA_ARGS__)

#define LOG_UI_INFO(formatStr, ...)                     \
{                                                       \
    StackStr(msg, n_, formatStr, __VA_ARGS__);          \
    App::Log(msg, App::LogMessage::INFO);               \
}

#define LOG_UI_WARNING(formatStr, ...)                  \
{                                                       \
    StackStr(msg, n_, formatStr, __VA_ARGS__);          \
    App::Log(msg, App::LogMessage::WARNING);            \
}
