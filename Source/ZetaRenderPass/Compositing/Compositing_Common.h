#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define COMPOSITING_THREAD_GROUP_DIM_X 8u
#define COMPOSITING_THREAD_GROUP_DIM_Y 8u

#define FIREFLY_FILTER_THREAD_GROUP_DIM_X 16u
#define FIREFLY_FILTER_THREAD_GROUP_DIM_Y 8u

namespace CB_COMPOSIT_FLAGS
{
	static constexpr uint32_t SUN_DI = 1 << 0;
	static constexpr uint32_t SKY_DI = 1 << 1;
	static constexpr uint32_t INDIRECT = 1 << 2;
	static constexpr uint32_t INSCATTERING = 1 << 4;
	static constexpr uint32_t EMISSIVE_DI = 1 << 5;
};

struct cbCompositing
{
	uint32_t CompositedUAVDescHeapIdx;
	uint32_t DiffuseDNSRCacheDescHeapIdx;
	uint32_t InscatteringDescHeapIdx;
	uint32_t SunShadowDescHeapIdx;
	uint32_t SpecularDNSRCacheDescHeapIdx;
	uint32_t SkyDIDenoisedDescHeapIdx;
	uint32_t EmissiveDIDenoisedDescHeapIdx;
	uint32_t IndirectDenoisedDescHeapIdx;

	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;

	uint32_t Flags;
};

struct cbFireflyFilter
{
	uint32_t CompositedUAVDescHeapIdx;
};

#endif