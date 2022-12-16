#ifndef STAD_COMMON_H
#define STAD_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_X 16
#define STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Y 16
#define STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Z 1

#define STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_X 8
#define STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y 8
#define STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Z 1

#define STAD_SPATIAL_TILE_WIDTH 8
#define STAD_SPATIAL_LOG2_TILE_WIDTH 3

struct cbSTADTemporalFilter
{
	uint32_t MaxTspp;
	float MaxPlaneDist;
	float BilinearNormalScale;
	float BilinearNormalExp;

	//
	// Resources
	//

	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;

	// previous temporal cache
	// Texture2D<half4>: (color, tspp)
	uint32_t PrevTemporalCacheDescHeapIdx;

	// current temporal cache
	// RWTexture2D<half4>: (color, tspp)
	uint32_t CurrTemporalCacheDescHeapIdx;

	uint32_t IsTemporalCacheValid;
};

struct cbSTADSpatialFilter
{
	uint32_t MaxTspp;
	float FilterRadiusBase;
	float FilterRadiusScale;
	float NormalExp;
	float MaxPlaneDist;

	uint32_t CurrPass;
	uint32_t NumPasses;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;  // == TileWidth * DispatchDimY

	uint32_t TemporalCacheInDescHeapIdx;
	uint32_t TemporalCacheOutDescHeapIdx;
};

#endif
