#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

struct FontSpan
{
    const void* Data;
    size_t N;
};

enum FONT_TYPE
{
    ROBOTO_REGULAR,
    FONT_AWESOME_6,
    BFONT,
    COUNT
};

__declspec(dllexport) FontSpan GetFont(FONT_TYPE f);

#if defined(__cplusplus)
}
#endif