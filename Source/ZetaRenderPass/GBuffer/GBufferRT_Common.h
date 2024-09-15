#ifndef GBUFFER_RT_COMMON_H
#define GBUFFER_RT_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define GBUFFER_RT_GROUP_DIM_X 8u
#define GBUFFER_RT_GROUP_DIM_Y 8u

#define GBUFFER_RT_TILE_WIDTH 16
#define GBUFFER_RT_LOG2_TILE_WIDTH 4

enum class UAV_DESC_TABLE
{
    BASE_COLOR,
    NORMAL,
    METALLIC_ROUGHNESS,
    MOTION_VECTOR,
    EMISSIVE,
    IOR,
    COAT,
    DEPTH,
    TRI_DIFF_GEO_A,
    TRI_DIFF_GEO_B,
    COUNT
};

struct cbGBufferRt
{
    uint32_t UavTableDescHeapIdx;

    uint16_t PickedPixelX;
    uint16_t PickedPixelY;
    uint16_t DispatchDimX;
    uint16_t DispatchDimY;
    uint16_t NumGroupsInTile;
    uint16_t pad;
};

#endif