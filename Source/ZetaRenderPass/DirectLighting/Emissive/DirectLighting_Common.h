#ifndef DIRECT_LIGHTING_COMMON_H
#define DIRECT_LIGHTING_COMMON_H

#include "../../../ZetaCore/Core/HLSLCompat.h"
#include "../../../ZetaCore/RayTracing/RtCommon.h"

#define ESTIMATE_TRI_LUMEN_GROUP_DIM_X 256u
#define ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI 64
#define ESTIMATE_TRI_LUMEN_WAVE_LEN 32
#define ESTIMATE_TRI_LUMEN_NUM_TRIS_PER_GROUP (ESTIMATE_TRI_LUMEN_GROUP_DIM_X / ESTIMATE_TRI_LUMEN_WAVE_LEN)

#define RESTIR_DI_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_DI_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_DI_TEMPORAL_TILE_WIDTH 16
#define RESTIR_DI_TEMPORAL_LOG2_TILE_WIDTH 4

#define RESTIR_DI_PRESAMPLE_GROUP_DIM_X 64u

#define RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_X 8u
#define RESTIR_DI_DNSR_TEMPORAL_GROUP_DIM_Y 8u

#define RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_X 8u
#define RESTIR_DI_DNSR_SPATIAL_GROUP_DIM_Y 8u

#define RESTIR_DI_DNSR_SPATIAL_TILE_WIDTH 16
#define RESTIR_DI_DNSR_SPATIAL_LOG2_TILE_WIDTH 4

// Given discrete probability distribution P with N outcomes, such that for outcome i and random variable x,
//      P[i] = P[x = i],
// 
// alias table is a lookup table of length N for P. To draw samples from P, draw a discrete uniform sample x 
// in [0, N), then
// 
// 1. draw another uniform sample u in [0, 1)
// 2. if u <= AliasTable[x].P_Curr, return x
// 3. return AliasTable[x].Alias
struct EmissiveTriangleSample
{
    // cache the probabilities for both outcomes to avoid another (random) memory access at the
	// cost of extra storage
	float CachedP_Orig;
	float CachedP_Alias;
	float P_Curr;
    uint32_t Alias;
};

struct LightSample
{
	float Pdf;
	uint32_t Index;

#ifdef __cplusplus
	ZetaRay::RT::EmissiveTriangle Tri;
#else
	RT::EmissiveTriangle Tri;
#endif
};

struct cb_ReSTIR_DI_EstimateTriLumen
{
	uint32_t TotalNumTris;
};

struct cb_ReSTIR_DI_Presampling
{
	uint32_t NumTotalSamples;
	uint32_t NumEmissiveTriangles;
};

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
