#pragma once

#include "App.h"

//#ifdef _DEBUG
#define LOG(formatStr, ...)					\
{											\
	ZetaRay::App::LockStdOut();				\
	printf(formatStr, __VA_ARGS__);			\
	ZetaRay::App::UnlockStdOut();			\
}
//#else
//#define LOG(formatStr, ...)		((void)0)
//#endif
