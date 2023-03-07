#ifndef STAD_COMMON_H
#define STAD_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_X 16
#define DiffuseDNSR_TEMPORAL_THREAD_GROUP_SIZE_Y 16

#define DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_X 8
#define DiffuseDNSR_SPATIAL_THREAD_GROUP_SIZE_Y 8

#define DiffuseDNSR_SPATIAL_TILE_WIDTH 8
#define DiffuseDNSR_SPATIAL_LOG2_TILE_WIDTH 3

struct cbDiffuseDNSRTemporal
{
	uint32_t MaxTspp;
	float BilinearNormalScale;
	float BilinearNormalExp;

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

struct cbDiffuseDNSRSpatial
{
	uint32_t MaxTspp;
	float FilterRadiusBase;
	float FilterRadiusScale;
	float NormalExp;

	uint32_t CurrPass;
	uint32_t NumPasses;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;  // == TileWidth * DispatchDimY

	uint32_t TemporalCacheInDescHeapIdx;
	uint32_t TemporalCacheOutDescHeapIdx;
};

#endif
