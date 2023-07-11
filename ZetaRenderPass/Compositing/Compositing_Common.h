#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define COMPOSITING_THREAD_GROUP_DIM_X 8
#define COMPOSITING_THREAD_GROUP_DIM_Y 8

#define FIREFLY_FILTER_THREAD_GROUP_DIM_X 16
#define FIREFLY_FILTER_THREAD_GROUP_DIM_Y 8

#define DOF_GATHER_THREAD_GROUP_DIM_X 8
#define DOF_GATHER_THREAD_GROUP_DIM_Y 8

#define GAUSSIAN_FILTER_THREAD_GROUP_DIM_X 8
#define GAUSSIAN_FILTER_THREAD_GROUP_DIM_Y 8

struct cbCompositing
{
	uint32_t CompositedUAVDescHeapIdx;
	uint32_t DiffuseDNSRCacheDescHeapIdx;
	uint32_t InscatteringDescHeapIdx;
	uint32_t SunShadowDescHeapIdx;
	uint32_t SpecularDNSRCacheDescHeapIdx;
	uint32_t DirectDNSRCacheDescHeapIdx;

	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;

	float FocalLength;
	float FStop;
	float FocusDepth;

	float RoughnessCutoff;
	uint16_t SunLighting;
	uint16_t SkyLighting;
	uint16_t DiffuseIndirect;
	uint16_t SpecularIndirect;
	uint16_t AccumulateInscattering;
};

struct cbFireflyFilter
{
	uint32_t CompositedSRVDescHeapIdx;
	uint32_t FilteredUAVDescHeapIdx;
	uint16_t DoFIsON;
};

struct cbDoF
{
	uint32_t InputDescHeapIdx;
	uint32_t OutputDescHeapIdx;
	float RadiusScale;
	float MaxBlurRadius;
	float MinLumToFilter;
	uint32_t IsGaussianFilterEnabled;
};

struct cbGaussianFilter
{
	uint32_t GatherSrvDescHeapIdx;
	uint32_t FilteredUavDescHeapIdx;
};

#endif // COMPOSITING_H