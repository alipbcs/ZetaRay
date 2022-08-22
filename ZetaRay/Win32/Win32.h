#pragma once

#define NOMINMAX
#define NODRAWTEXT
#define NOGDI
#define NOBITMAP
#define NOMCX
#define NOSERVICE
#define NOHELP
#define NOIME
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// from <Windowsx.h>
#define GET_X_LPARAM(lp)           ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp)           ((int)(short)HIWORD(lp))