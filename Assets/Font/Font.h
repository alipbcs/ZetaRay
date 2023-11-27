#pragma once

#if defined(__cplusplus)
extern "C" {
#endif // #if defined(__cplusplus)

struct FontSpan
{
	const void* Data;
	size_t N;
};

enum FONT_TYPE
{
	ROBOTO_REGULAR,
	FONT_AWESOME_6,
	COUNT
};

__declspec(dllexport) FontSpan GetFont(FONT_TYPE f);

#if defined(__cplusplus)
}
#endif // #if defined(__cplusplus)