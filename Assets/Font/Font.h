#pragma once

#if defined(__cplusplus)
extern "C" {
#endif // #if defined(__cplusplus)

struct FontSpan
{
	const char* Data;
	size_t N;
};

enum FONT_TYPE
{
	SEGOE_UI,
	ROBOTO_REGULAR,
	DOMINE_MEDIUM,
	DEJAVU_SANS,
	COUNT
};

__declspec(dllexport) FontSpan GetFont(FONT_TYPE f);

#if defined(__cplusplus)
}
#endif // #if defined(__cplusplus)