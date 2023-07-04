#ifndef SKY_DI_COMMON_H
#define SKY_DI_COMMON_H

#include "../../../ZetaCore/Core/HLSLCompat.h"

#define SKY_DI_TEMPORAL_GROUP_DIM_X 16
#define SKY_DI_TEMPORAL_GROUP_DIM_Y 8

#define SKY_DI_TEMPORAL_TILE_WIDTH 8
#define SKY_DI_TEMPORAL_LOG2_TILE_WIDTH 3

#define SKY_DI_SPATIAL_GROUP_DIM_X 16
#define SKY_DI_SPATIAL_GROUP_DIM_Y 16

#define SKY_DI_SPATIAL_TILE_WIDTH 8
#define SKY_DI_SPATIAL_LOG2_TILE_WIDTH 3

#define SKY_DI_DNSR_TEMPORAL_GROUP_DIM_X 16
#define SKY_DI_DNSR_TEMPORAL_GROUP_DIM_Y 8

#define SKY_DI_DNSR_SPATIAL_GROUP_DIM_X 16
#define SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y 16

#define SKY_DI_DNSR_SPATIAL_TILE_WIDTH 16
#define SKY_DI_DNSR_SPATIAL_LOG2_TILE_WIDTH 4

struct cb_SkyDI_Temporal
{
	uint32_t PrevTemporalReservoir_A_DescHeapIdx;
	uint32_t CurrTemporalReservoir_A_DescHeapIdx;
	uint32_t CurrTemporalReservoir_B_DescHeapIdx;
	
	float MinRoughnessResample;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;
	uint16_t DoTemporalResampling;
	uint16_t CheckerboardTracing;
	uint16_t M_max;
	uint16_t PrefilterReservoirs;
	uint16_t SampleIndex;
};

struct cb_SkyDI_Spatial
{
	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;
	uint32_t OutputReservoir_A_DescHeapIdx;

	float MinRoughnessResample;
	uint16_t DoSpatialResampling;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;
	uint16_t CheckerboardTracing;
};

struct cb_SkyDI_DNSR_Temporal
{
	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputRISEstimate_DescHeapIdx;
	uint32_t PrevTemporalCacheDiffuseDescHeapIdx;
	uint32_t CurrTemporalCacheDiffuseDescHeapIdx;
	uint32_t PrevTemporalCacheSpecularDescHeapIdx;
	uint32_t CurrTemporalCacheSpecularDescHeapIdx;
	uint32_t FinalDescHeapIdx;

	float MinRoughnessResample;
	uint16_t Denoise;
	uint16_t IsTemporalCacheValid;
	uint16_t MaxTSPP_Diffuse;
	uint16_t MaxTSPP_Specular;
};

struct cb_SkyDI_DNSR_Spatial
{
	uint32_t CurrTemporalCacheDiffuseDescHeapIdx;
	uint32_t CurrTemporalCacheSpecularDescHeapIdx;
	uint32_t FinalDescHeapIdx;
	float MinRoughnessResample;

	uint16_t Denoise;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;
	uint16_t MaxTSPP;
	uint16_t FilterDiffuse;
	uint16_t FilterSpecular;
};

#endif
