#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8

struct cbCompositing
{
	uint32_t CompositedUAVDescHeapIdx;
	uint32_t DiffuseDNSRCacheDescHeapIdx;
	uint32_t InscatteringDescHeapIdx;
	uint32_t SunShadowDescHeapIdx;
	uint32_t SpecularDNSRCacheDescHeapIdx;

	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;

	uint16_t DirectLighting;
	uint16_t IndirectDiffuse;
	uint16_t IndirectSpecular;
	uint16_t AccumulateInscattering;
	float RoughnessCutoff;

	float FocalLength;
	float FStop;
	float FocusDepth;
};

struct cbDoF
{
	uint32_t GatherUAVDescHeapIdx;
	uint32_t CompositedSRVDescHeapIdx;
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