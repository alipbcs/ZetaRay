#ifndef INDIRECT_LIGHTING_COMMON_H
#define INDIRECT_LIGHTING_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"
#include "../../ZetaCore/RayTracing/RtCommon.h"

#define RESTIR_GI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_GI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_GI_TEMPORAL_TILE_WIDTH 16
#define RESTIR_GI_TEMPORAL_LOG2_TILE_WIDTH 4

#define INDIRECT_DNSR_TEMPORAL_GROUP_DIM_X 8u
#define INDIRECT_DNSR_TEMPORAL_GROUP_DIM_Y 8u

#define INDIRECT_DNSR_SPATIAL_GROUP_DIM_X 8u
#define INDIRECT_DNSR_SPATIAL_GROUP_DIM_Y 8u

#define INDIRECT_DNSR_SPATIAL_TILE_WIDTH 16
#define INDIRECT_DNSR_SPATIAL_LOG2_TILE_WIDTH 4

namespace CB_IND_FLAGS
{
    static constexpr uint32_t TEMPORAL_RESAMPLE = 1 << 0;
    static constexpr uint32_t SPATIAL_RESAMPLE = 1 << 1;
    static constexpr uint32_t STOCHASTIC_MULTI_BOUNCE = 1 << 2;
    static constexpr uint32_t RUSSIAN_ROULETTE = 1 << 3;
    static constexpr uint32_t DENOISE = 1 << 4;
};

struct cb_ReSTIR_GI_SpatioTemporal
{
    uint32_t PrevReservoir_A_DescHeapIdx;
    uint32_t PrevReservoir_B_DescHeapIdx;
    uint32_t PrevReservoir_C_DescHeapIdx;
    uint32_t CurrReservoir_A_DescHeapIdx;
    uint32_t CurrReservoir_B_DescHeapIdx;
    uint32_t CurrReservoir_C_DescHeapIdx;
    uint32_t CurvatureDescHeapIdx;

    uint32_t FinalOrColorAUavDescHeapIdx;
    uint32_t ColorBUavDescHeapIdx;

    float Extents_x;
    float Extents_y;
    float Extents_z;
    float Offset_y;

    uint32_t Flags;

    uint32_t SampleSetSize;
    uint32_t NumSampleSets;
    uint16_t M_max;
    uint16_t NumBounces;
    uint16_t GridDim_x;
    uint16_t GridDim_y;
    uint16_t GridDim_z;
    uint16_t DispatchDimX;
    uint16_t NumGroupsInTile;
    uint16_t pad;
};

struct cbIndirectDnsrTemporal
{
    uint32_t ColorASrvDescHeapIdx;
    uint32_t ColorBSrvDescHeapIdx;
    uint32_t PrevTemporalCacheDiffuseDescHeapIdx;
    uint32_t CurrTemporalCacheDiffuseDescHeapIdx;
    uint32_t PrevTemporalCacheSpecularDescHeapIdx;
    uint32_t CurrTemporalCacheSpecularDescHeapIdx;
    uint32_t CurvatureDescHeapIdx;
    uint32_t PrevReservoir_A_DescHeapIdx;

    uint32_t MaxTsppDiffuse;
    uint32_t MaxTsppSpecular;
    uint16_t Denoise;
    uint16_t IsTemporalCacheValid;
    uint16_t FilterFirefly;
    uint16_t pad;
};

struct cbIndirectDnsrSpatial
{
    uint32_t TemporalCacheDiffuseDescHeapIdx;
    uint32_t TemporalCacheSpecularDescHeapIdx;
    uint32_t ColorBSrvDescHeapIdx;
    uint32_t FinalDescHeapIdx;

    uint16_t Denoise;
    uint16_t DispatchDimX;
    uint16_t DispatchDimY;
    uint16_t NumGroupsInTile;
    uint16_t MaxTsppDiffuse;
    uint16_t MaxTsppSpecular;
    uint16_t FilterDiffuse;
    uint16_t FilterSpecular;
};

#endif
