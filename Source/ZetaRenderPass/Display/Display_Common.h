#ifndef DISPLAY_PASS_H
#define DISPLAY_PASS_H

#include "../../ZetaCore/Core/HLSLCompat.h"

enum class DisplayOption
{
    DEFAULT,
    BASE_COLOR,
    NORMAL,
    METALNESS_ROUGHNESS,
    ROUGHNESS_TH,
    EMISSIVE,
    TRANSMISSION,
    DEPTH,
    COUNT
};

enum class Tonemapper
{
    NONE,
    NEUTRAL,
    AgX_DEFAULT,
    AgX_GOLDEN,
    AgX_PUNCHY,
    AgX_CUSTOM,
    COUNT
};

struct cbDisplayPass
{
    uint16_t DisplayOption;
    uint16_t Tonemapper;
    uint16_t AutoExposure;
    uint16_t pad;

    uint32_t InputDescHeapIdx;
    uint32_t ExposureDescHeapIdx;
    uint32_t LUTDescHeapIdx;

    float Saturation;
    float AgXExp;
    float RoughnessTh;
};

struct cbDrawPicked
{
    float4_ row0;
    float4_ row1;
    float4_ row2;
    float4_ row3;
};

struct cbSobel
{
    uint32_t MaskDescHeapIdx;
};

#endif
