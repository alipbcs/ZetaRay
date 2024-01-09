#ifndef GBUFFER_RT_COMMON_H
#define GBUFFER_RT_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define GBUFFER_RT_GROUP_DIM_X 8u
#define GBUFFER_RT_GROUP_DIM_Y 8u

#define GBUFFER_RT_TILE_WIDTH 16
#define GBUFFER_RT_LOG2_TILE_WIDTH 4

struct cbGBufferRt
{
    uint32_t BaseColorUavDescHeapIdx;
    uint32_t NormalUavDescHeapIdx;
    uint32_t MetallicRoughnessUavDescHeapIdx;
    uint32_t EmissiveColorUavDescHeapIdx;
    uint32_t MotionVectorUavDescHeapIdx;
    uint32_t TransmissionUavDescHeapIdx;
    uint32_t DepthUavDescHeapIdx;

    uint16_t DispatchDimX;
    uint16_t DispatchDimY;
    uint16_t NumGroupsInTile;
    uint16_t pad;
};

#endif