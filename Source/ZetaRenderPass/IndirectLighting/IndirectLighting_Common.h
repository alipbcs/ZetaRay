#ifndef INDIRECT_LIGHTING_COMMON_H
#define INDIRECT_LIGHTING_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define RESTIR_GI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_GI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_GI_TEMPORAL_TILE_WIDTH 16
#define RESTIR_GI_TEMPORAL_LOG2_TILE_WIDTH 4

#define RESTIR_PT_PATH_TRACE_GROUP_DIM_X 16u
#define RESTIR_PT_PATH_TRACE_GROUP_DIM_Y 8u

#define RESTIR_PT_TILE_WIDTH 16
#define RESTIR_PT_LOG2_TILE_WIDTH 4

#define RESTIR_PT_TEMPORAL_GROUP_DIM_X 16u
#define RESTIR_PT_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_PT_REPLAY_GROUP_DIM_X 16u
#define RESTIR_PT_REPLAY_GROUP_DIM_Y 8u

#define RESTIR_PT_SORT_GROUP_DIM_X 16u
#define RESTIR_PT_SORT_GROUP_DIM_Y 16u
#define LOG_RESTIR_PT_SORT_GROUP_DIM 4

#define RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_X 8u
#define RESTIR_PT_SPATIAL_SEARCH_GROUP_DIM_Y 8u

#define RESTIR_PT_SPATIAL_GROUP_DIM_X 8u
#define RESTIR_PT_SPATIAL_GROUP_DIM_Y 8u

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
};

enum class RPT_DEBUG_VIEW
{
    NONE,
    K,
    CASE,
    FOUND_CONNECTION,
    CONNECTION_LOBE_K_MIN_1,
    CONNECTION_LOBE_K,
    COUNT
};

enum class TEXTURE_FILTER
{
    MIP0,
    TRI_LINEAR,
    ANISOTROPIC_2X,
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

    uint32_t FinalDescHeapIdx;

    uint32_t Flags;
    uint32_t DispatchDimX_NumGroupsInTile;
    uint32_t SampleSetSize_NumSampleSets;
    uint32_t Extents_xy;
    uint32_t Extents_z_Offset_y;
    uint32_t GridDim_xy;
    uint32_t GridDim_z;
 
    uint32_t M_max;
    uint32_t MaxNonTrBounces;
    uint32_t MaxGlossyTrBounces;
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

#endif
