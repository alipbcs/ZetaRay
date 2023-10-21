#ifndef SKY_DI_COMMON_H
#define SKY_DI_COMMON_H

#include "../../../ZetaCore/Core/HLSLCompat.h"

#define SKY_DI_TEMPORAL_GROUP_DIM_X 16u
#define SKY_DI_TEMPORAL_GROUP_DIM_Y 8u

#define SKY_DI_TEMPORAL_TILE_WIDTH 8
#define SKY_DI_TEMPORAL_LOG2_TILE_WIDTH 3

#define SKY_DI_DNSR_TEMPORAL_GROUP_DIM_X 16u
#define SKY_DI_DNSR_TEMPORAL_GROUP_DIM_Y 8u

#define SKY_DI_DNSR_SPATIAL_GROUP_DIM_X 16u
#define SKY_DI_DNSR_SPATIAL_GROUP_DIM_Y 16u

#define SKY_DI_DNSR_SPATIAL_TILE_WIDTH 16
#define SKY_DI_DNSR_SPATIAL_LOG2_TILE_WIDTH 4

namespace CB_SKY_DI_FLAGS
{
	static constexpr uint32_t TEMPORAL_RESAMPLE = 1 << 0;
	static constexpr uint32_t SPATIAL_RESAMPLE = 1 << 1;
	static constexpr uint32_t DENOISE = 1 << 2;
	//static constexpr uint32_t CHECKERBOARDING = 1 << 3;
};

namespace CB_SKY_DI_DNSR_TEMPORAL_FLAGS
{
	static constexpr uint32_t DENOISE = 1 << 0;
	static constexpr uint32_t CACHE_VALID = 1 << 1;
};

namespace CB_SKY_DI_DNSR_SPATIAL_FLAGS
{
	static constexpr uint32_t DENOISE = 1 << 0;
	static constexpr uint32_t FILTER_DIFFUSE = 1 << 1;
	static constexpr uint32_t FILTER_SPECULAR = 1 << 2;
};

struct cb_SkyDI_Temporal
{
	uint32_t PrevReservoir_A_DescHeapIdx;
	uint32_t PrevReservoir_B_DescHeapIdx;
	uint32_t CurrReservoir_A_DescHeapIdx;
	uint32_t CurrReservoir_B_DescHeapIdx;
	uint32_t ColorAUavDescHeapIdx;
	uint32_t ColorBUavDescHeapIdx;
	uint32_t FinalDescHeapIdx;
	
	float MinRoughnessResample;
	uint32_t Flags;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;
	uint16_t M_max;
};

struct cb_SkyDI_DNSR_Temporal
{
	uint32_t ColorASrvDescHeapIdx;
	uint32_t ColorBSrvDescHeapIdx;
	uint32_t PrevTemporalCacheDiffuseDescHeapIdx;
	uint32_t CurrTemporalCacheDiffuseDescHeapIdx;
	uint32_t PrevTemporalCacheSpecularDescHeapIdx;
	uint32_t CurrTemporalCacheSpecularDescHeapIdx;
	float MinRoughnessResample;

	uint32_t Flags;
	uint16_t MaxTsppDiffuse;
	uint16_t MaxTsppSpecular;
};

struct cb_SkyDI_DNSR_Spatial
{
	uint32_t TemporalCacheDiffuseDescHeapIdx;
	uint32_t TemporalCacheSpecularDescHeapIdx;
	uint32_t ColorBSrvDescHeapIdx;
	uint32_t FinalDescHeapIdx;
	float MinRoughnessResample;

	uint32_t Flags;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumGroupsInTile;
	uint16_t MaxTsppDiffuse;
	uint16_t MaxTsppSpecular;
};

#endif
