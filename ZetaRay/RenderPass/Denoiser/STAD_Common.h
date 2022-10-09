#ifndef STAD_COMMON_H
#define STAD_COMMON_H

#include "../Common/HLSLCompat.h"

#define STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_X 8
#define STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Y 8
#define STAD_TEMPORAL_PASS_THREAD_GROUP_SIZE_Z 1

#define STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_X 8
#define STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y 8
#define STAD_SPATIAL_FILTER_THREAD_GROUP_SIZE_Z 1

struct cbSTADTemporalFilter
{
	uint32_t MaxTspp;
	float MaxPlaneDist;
	float BilinearNormalScale;
	float BilinearNormalExp;

	//
	// Resources
	//

	// Incoming indirect light L_i for each surface position (element w is rayhitdist) in
	// the gbuffer. Note that is indirect light being received by the surface pos rather than the 
	// indirect light reflected back towards the viewer (due to indirect illumination L_i). Reason 
	// for doing so is that shading involves texture details that make denoising more difficult. Therfore,
	// shading is performed after denoising (here shading is just multiplication by Lambert's BRDF)
	// Texture2D<half4>
	uint32_t IndirectLiRayTDescHeapIdx;

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
	uint16_t TileWidth;		// must be a power of 2
	uint16_t Log2TileWidth;
	uint16_t NumGroupsInTile;  // == TileWidth * DispatchDimY

	uint32_t TemporalCacheInDescHeapIdx;
	uint32_t TemporalCacheOutDescHeapIdx;
};

#ifndef __cplusplus

// Ref: Christensen et al, "Progressive multi-jittered sample sequences"
static const float2 k_pmjbn[] =
{
	float2(0.1638801546617692, 0.2880264570633905),
	float2(-0.34337816638748414, -0.21086168504748115),
	float2(0.25428317450586535, -0.2659005760211397),
	float2(-0.23356076829756228, 0.2080983361991393),
	float2(0.493311861389224, 0.1212089705044751),
	float2(-0.027024366409327927, -0.39108611271966465),
	float2(0.0053965933575517155, -0.0337609977315011),
	float2(-0.46900909265281177, 0.49429306245906046)
};

// for sample (x, y) where -0.5 <= x, y <= 0.5, sqrt(x^2 + y^2) gives distance from (0, 0)
static const float k_sampleDist[] =
{
	0.3313848896079218,
	0.4029531180828533,
	0.3679171770455333,
	0.3128187174972073,
	0.5079844555870344,
	0.3920187035614549,
	0.03418959180354737,
	0.6813920755234613
};

#endif // __cplusplus

#endif
