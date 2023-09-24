#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define COMPOSITING_THREAD_GROUP_DIM_X 8u
#define COMPOSITING_THREAD_GROUP_DIM_Y 8u

#define FIREFLY_FILTER_THREAD_GROUP_DIM_X 16u
#define FIREFLY_FILTER_THREAD_GROUP_DIM_Y 8u

struct cbCompositing
{
	uint32_t CompositedUAVDescHeapIdx;
	uint32_t DiffuseDNSRCacheDescHeapIdx;
	uint32_t InscatteringDescHeapIdx;
	uint32_t SunShadowDescHeapIdx;
	uint32_t SpecularDNSRCacheDescHeapIdx;
	uint32_t SkyDIDenoisedDescHeapIdx;
	uint32_t EmissiveDIDenoisedDescHeapIdx;

	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;

	float RoughnessCutoff;
	uint16_t SunLighting;
	uint16_t SkyLighting;
	uint16_t DiffuseIndirect;
	uint16_t SpecularIndirect;
	uint16_t AccumulateInscattering;
	uint16_t EmissiveLighting;
};

struct cbFireflyFilter
{
	uint32_t CompositedUAVDescHeapIdx;
};

#endif