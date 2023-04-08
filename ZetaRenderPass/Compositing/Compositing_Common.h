#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8
#define THREAD_GROUP_SIZE_Z 1

struct cbCompositing
{
	// Texture2D<float4>
	uint32_t HDRLightAccumDescHeapIdx;

	// current frame's integrated values
	// Texture2D<half4>: (tspp | color.r >> 16, color.g | color.b >> 16, lum | lum^2 >> 16)
	uint32_t DenoiserTemporalCacheDescHeapIdx;

	// Texture3D<half4>
	uint32_t InscatteringDescHeapIdx;

	uint32_t AccumulateInscattering;
	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;

	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;

	uint16_t DisplayDirectLightingOnly;
	uint16_t DisplayIndirectDiffuseOnly;
	uint16_t UseRawIndirectDiffuse;
	uint16_t SkipLighting;

	uint32_t SunShadowDescHeapIdx;
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