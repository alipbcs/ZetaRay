#ifndef DIRECT_LIGHTING_COMMON_H
#define DIRECT_LIGHTING_COMMON_H

#include "../../../ZetaCore/RayTracing/RtCommon.h"

#define RESTIR_DI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_DI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_DI_TILE_WIDTH 16
#define RESTIR_DI_LOG2_TILE_WIDTH 4

namespace CB_RDI_FLAGS
{
    static constexpr uint32_t TEMPORAL_RESAMPLE = 1 << 0;
    static constexpr uint32_t SPATIAL_RESAMPLE = 1 << 1;
    static constexpr uint32_t STOCHASTIC_SPATIAL = 1 << 2;
    static constexpr uint32_t EXTRA_DISOCCLUSION_SAMPLING = 1 << 3;
};

struct cb_ReSTIR_DI
{
    uint32_t PrevReservoir_A_DescHeapIdx;
    uint32_t CurrReservoir_A_DescHeapIdx;

    uint32_t TargetDescHeapIdx;
    uint32_t FinalDescHeapIdx;

    uint32_t Flags;
    uint32_t M_max;
    uint32_t NumSampleSets;
    uint32_t SampleSetSize;
    uint16_t DispatchDimX;
    uint16_t DispatchDimY;
    uint16_t NumGroupsInTile;
    uint16_t pad;
};

#endif
