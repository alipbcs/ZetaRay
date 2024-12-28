#ifndef SKY_DI_COMMON_H
#define SKY_DI_COMMON_H

#include "../../../ZetaCore/Core/HLSLCompat.h"

#define SKY_DI_GROUP_DIM_X 8u
#define SKY_DI_GROUP_DIM_Y 8u

#define SKY_DI_TILE_WIDTH 8
#define SKY_DI_LOG2_TILE_WIDTH 3

namespace CB_SKY_DI_FLAGS
{
    static constexpr uint32_t TEMPORAL_RESAMPLE = 1 << 0;
    static constexpr uint32_t SPATIAL_RESAMPLE = 1 << 1;
    static constexpr uint32_t RESET_TEMPORAL_TEXTURES = 1 << 2;
};

struct cb_SkyDI
{
    uint32_t PrevReservoir_A_DescHeapIdx;
    uint32_t CurrReservoir_A_DescHeapIdx;
    uint32_t TargetDescHeapIdx;
    uint32_t FinalDescHeapIdx;

    float Alpha_min;
    uint32 M_max;
    uint32_t Flags;
    uint16_t DispatchDimX;
    uint16_t DispatchDimY;
    uint16_t NumGroupsInTile;
    uint16_t pad;
};

#endif
