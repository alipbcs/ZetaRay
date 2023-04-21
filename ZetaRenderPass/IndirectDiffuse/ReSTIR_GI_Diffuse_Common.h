#ifndef RESTIR_GI_DIFF_COMMON_H
#define RESTIR_GI_DIFF_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define RGI_DIFF_TEMPORAL_GROUP_DIM_X 16
#define RGI_DIFF_TEMPORAL_GROUP_DIM_Y 8

#define RGI_DIFF_SPATIAL_GROUP_DIM_X 32
#define RGI_DIFF_SPATIAL_GROUP_DIM_Y 32

#define RGI_DIFF_TEMPORAL_TILE_WIDTH 16
#define RGI_DIFF_TEMPORAL_LOG2_TILE_WIDTH 4

#define RGI_DIFF_SPATIAL_TILE_WIDTH 16
#define RGI_DIFF_SPATIAL_LOG2_TILE_WIDTH 4

#define DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_X 16
#define DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_Y 16

#define DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X 32
#define DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y 16

#define DiffuseDNSR_SPATIAL_TILE_WIDTH 16
#define DiffuseDNSR_SPATIAL_LOG2_TILE_WIDTH 4

struct cb_RGI_Diff_Temporal
{
	uint32_t FrameCounter;
	uint32_t PrevTemporalReservoir_A_DescHeapIdx;
	uint32_t PrevTemporalReservoir_B_DescHeapIdx;
	uint32_t PrevTemporalReservoir_C_DescHeapIdx;

	uint32_t CurrTemporalReservoir_A_DescHeapIdx;
	uint32_t CurrTemporalReservoir_B_DescHeapIdx;
	uint32_t CurrTemporalReservoir_C_DescHeapIdx;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;

	uint16_t NumGroupsInTile;
	uint16_t IsTemporalReservoirValid;
	uint16_t DoTemporalResampling;
	uint16_t PdfCorrection;

	uint16_t SampleIndex;
	uint16_t CheckerboardTracing;
};

struct cb_RGI_Diff_Spatial
{
	float NormalExp;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;

	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;
	uint32_t InputReservoir_C_DescHeapIdx;
	uint32_t OutputReservoir_A_DescHeapIdx;
	uint32_t OutputReservoir_B_DescHeapIdx;
	uint32_t OutputReservoir_C_DescHeapIdx;
	uint16_t NumGroupsInTile;
	uint16_t PdfCorrection;
	uint16_t IsFirstPass;
	uint16_t DoSpatialResampling;
	uint16_t Radius1st;
	uint16_t Radius2nd;
};

struct cbDiffuseDNSRTemporal
{
	uint16_t MaxTspp;
	uint16_t IsTemporalCacheValid;

	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;

	// previous temporal cache
	// Texture2D<half4>: (color, tspp)
	uint32_t PrevTemporalCacheDescHeapIdx;

	// current temporal cache
	// RWTexture2D<half4>: (color, tspp)
	uint32_t CurrTemporalCacheDescHeapIdx;
};

struct cbDiffuseDNSRSpatial
{
	float NormalExp;
	float FilterRadiusScale;

	uint16_t CurrPass;
	uint16_t NumPasses;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;  // == TileWidth * DispatchDimY
	uint16_t MaxTspp;
	uint16_t MinFilterRadius;
	uint16_t MaxFilterRadius;

	uint32_t TemporalCacheInDescHeapIdx;
	uint32_t TemporalCacheOutDescHeapIdx;
};

#endif
