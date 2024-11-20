#ifndef DIRECT_LIGHTING_COMMON_H
#define DIRECT_LIGHTING_COMMON_H

#include "../../../ZetaCore/RayTracing/RtCommon.h"

#define RESTIR_DI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_DI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_DI_TILE_WIDTH 16
#define RESTIR_DI_LOG2_TILE_WIDTH 4

struct cb_ReSTIR_DI_SpatioTemporal
{
    uint32_t PrevReservoir_A_DescHeapIdx;
    uint32_t PrevReservoir_B_DescHeapIdx;
    uint32_t CurrReservoir_A_DescHeapIdx;
    uint32_t CurrReservoir_B_DescHeapIdx;
    uint32_t FinalDescHeapIdx;

    uint16_t DispatchDimX;
    uint16_t DispatchDimY;
    uint16_t NumGroupsInTile;
    uint16_t TemporalResampling;
    uint16_t M_max;
    uint16_t NumSampleSets;
    uint16_t SampleSetSize;
    uint16_t SpatialResampling;
    uint16_t ExtraSamplesDisocclusion;
    uint16_t StochasticSpatial;
    uint16_t pad;
};

#endif
