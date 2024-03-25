#ifndef INDIRECT_LIGHTING_COMMON_H
#define INDIRECT_LIGHTING_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define RESTIR_GI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_GI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_GI_TEMPORAL_TILE_WIDTH 16
#define RESTIR_GI_TEMPORAL_LOG2_TILE_WIDTH 4

#define RESTIR_PT_PATH_TRACE_GROUP_DIM_X 8u
#define RESTIR_PT_PATH_TRACE_GROUP_DIM_Y 8u

#define RESTIR_PT_TILE_WIDTH 16
#define RESTIR_PT_LOG2_TILE_WIDTH 4

#define RESTIR_PT_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_PT_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_PT_REPLAY_GROUP_DIM_X 8u
#define RESTIR_PT_REPLAY_GROUP_DIM_Y 8u

#define RESTIR_PT_SORT_GROUP_DIM_X 32u
#define RESTIR_PT_SORT_GROUP_DIM_Y 32u

#define RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X 8u
#define RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y 8u

#define RESTIR_PT_SPATIAL_GROUP_DIM_X 8u
#define RESTIR_PT_SPATIAL_GROUP_DIM_Y 8u

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
    static constexpr uint32_t BOILING_SUPPRESSION = 1 << 5;
    static constexpr uint32_t PATH_REGULARIZATION = 1 << 6;
    static constexpr uint32_t SORT_TEMPORAL = 1 << 7;
    static constexpr uint32_t SORT_SPATIAL = 1 << 8;
    static constexpr uint32_t REJECT_OUTLIERS = 1 << 9;
};

enum class RPT_DEBUG_VIEW
{
    NONE,
    K,
    CASE,
    FOUND_CONNECTION,
    RECONNECTION_LOBE,
    COUNT
};

enum class TEXTURE_FILTER
{
    MIP0,
    TRI_LINEAR,
    ANISOTROPIC_4X,
    ANISOTROPIC_16X,
    COUNT
};

struct cb_ReSTIR_GI
{
    uint32_t PrevReservoir_A_DescHeapIdx;
    uint32_t PrevReservoir_B_DescHeapIdx;
    uint32_t PrevReservoir_C_DescHeapIdx;
    uint32_t CurrReservoir_A_DescHeapIdx;
    uint32_t CurrReservoir_B_DescHeapIdx;
    uint32_t CurrReservoir_C_DescHeapIdx;

    uint32_t FinalOrColorAUavDescHeapIdx;
    uint32_t ColorBUavDescHeapIdx;

    uint32_t Flags;
    uint32_t DispatchDimX_NumGroupsInTile;
    uint32_t SampleSetSize_NumSampleSets;
    uint32_t Extents_xy;
    uint32_t Extents_z_Offset_y;
    uint32_t GridDim_xy;
    uint32_t GridDim_z;
 
    uint32_t M_max;
    uint32_t MaxDiffuseBounces;
    uint32_t MaxGlossyBounces_NonTr;
    uint32_t MaxGlossyBounces_Tr;
    uint32_t TexFilterDescHeapIdx;
};

struct cb_ReSTIR_PT_PathTrace
{
    uint32_t Reservoir_A_DescHeapIdx;
    uint32_t TargetDescHeapIdx;
    uint32_t Final;

    uint32_t Flags;
    uint32_t DispatchDimX_NumGroupsInTile;
    uint32_t SampleSetSize_NumSampleSets;
 
    uint32_t Packed;
    float Alpha_min;
    uint32_t TexFilterDescHeapIdx;
};

struct cb_ReSTIR_PT_Reuse
{
    // Since reservoir descriptors were allocated consecutively, knowing
    // the desc. heap index for A, others can be derived as:
    //  - B = A + 1
    //  - C = A + 2
    //  - D = A + 3
    //  - E = A + 4
    //  - F = A + 5
    uint32_t PrevReservoir_A_DescHeapIdx;
    uint32_t Reservoir_A_DescHeapIdx;
    uint32_t ThreadMap_CtN_DescHeapIdx;
    uint32_t ThreadMap_NtC_DescHeapIdx;
    uint32_t RBufferA_CtN_DescHeapIdx;
    uint32_t RBufferA_NtC_DescHeapIdx;
    uint32_t SpatialNeighborHeapIdx;
    uint32_t TargetDescHeapIdx;
    uint32_t Final;

    uint32_t Flags;
    uint32_t DispatchDimX_NumGroupsInTile;
 
    // DebugView << 20 | M_max << 16 | #Spatial << 14 | SpatialPass << 12 | Glossy << 8 | Glossy Refl << 4 | Diffuse
    uint32_t Packed;
    float Alpha_min;
    uint32_t TexFilterDescHeapIdx;
    uint32_t MaxSpatialM;
};

struct cb_ReSTIR_PT_Sort
{
    uint32_t Reservoir_A_DescHeapIdx;
    uint32_t SpatialNeighborHeapIdx;
    uint32_t MapDescHeapIdx;
    uint32_t Flags;

    uint32_t DispatchDimX;
    uint32_t DispatchDimY;
};

struct cb_ReSTIR_PT_SpatialSearch
{
    uint32_t DispatchDimX_NumGroupsInTile;
    uint32_t Packed;
    uint32_t OutputDescHeapIdx;
    uint32_t Flags;
    uint32_t Final;
};

struct cbIndirectDnsrTemporal
{
    uint32_t ColorASrvDescHeapIdx;
    uint32_t ColorBSrvDescHeapIdx;
    uint32_t PrevTemporalCacheDiffuseDescHeapIdx;
    uint32_t CurrTemporalCacheDiffuseDescHeapIdx;
    uint32_t PrevTemporalCacheSpecularDescHeapIdx;
    uint32_t CurrTemporalCacheSpecularDescHeapIdx;
    uint32_t PrevReservoir_A_DescHeapIdx;

    uint32_t MaxTsppDiffuse;
    uint32_t MaxTsppSpecular;
    uint16_t Denoise;
    uint16_t IsTemporalCacheValid;
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
