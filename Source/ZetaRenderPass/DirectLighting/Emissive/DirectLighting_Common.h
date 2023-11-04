#ifndef DIRECT_LIGHTING_COMMON_H
#define DIRECT_LIGHTING_COMMON_H

#include "../../../ZetaCore/RayTracing/RtCommon.h"

#define RESTIR_DI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_DI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_DI_TEMPORAL_TILE_WIDTH 16
#define RESTIR_DI_TEMPORAL_LOG2_TILE_WIDTH 4

#define RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_X 8u
#define RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_Y 8u

#define RESTIR_DI_DNSR_SPATIAL_TILE_WIDTH 16
#define RESTIR_DI_DNSR_SPATIAL_LOG2_TILE_WIDTH 4

struct cb_ReSTIR_DI_SpatioTemporal
{
	uint32_t NumEmissiveTriangles;
	float OneDivNumEmissiveTriangles;

	uint32_t PrevReservoir_A_DescHeapIdx;
	uint32_t PrevReservoir_B_DescHeapIdx;
	uint32_t CurrReservoir_A_DescHeapIdx;
	uint32_t CurrReservoir_B_DescHeapIdx;
	
	uint32_t ColorAUavDescHeapIdx;
	uint32_t ColorBUavDescHeapIdx;
	uint32_t FinalDescHeapIdx;

	float MaxRoughnessExtraBrdfSampling;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;
	uint16_t TemporalResampling;
	uint16_t M_max;
	uint16_t NumSampleSets;
	uint16_t SampleSetSize;
	uint16_t SpatialResampling;
	uint16_t Denoise;
};

struct cb_ReSTIR_DI_DNSR_Temporal
{
	uint32_t ColorASrvDescHeapIdx;
	uint32_t ColorBSrvDescHeapIdx;
	uint32_t PrevTemporalCacheDiffuseDescHeapIdx;
	uint32_t CurrTemporalCacheDiffuseDescHeapIdx;
	uint32_t PrevTemporalCacheSpecularDescHeapIdx;
	uint32_t CurrTemporalCacheSpecularDescHeapIdx;

	uint16_t Denoise;
	uint16_t IsTemporalCacheValid;
	uint16_t MaxTsppDiffuse;
	uint16_t MaxTsppSpecular;
	//uint16_t FilterFirefly;
};

struct cb_ReSTIR_DI_DNSR_Spatial
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
