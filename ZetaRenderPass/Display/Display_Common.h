#ifndef FINAL_PASS_H
#define FINAL_PASS_H

#include "../../ZetaCore/Core/HLSLCompat.h"

enum class DisplayOption
{
	DEFAULT,
	BASE_COLOR,
	NORMAL,
	METALNESS_ROUGHNESS,
	DEPTH,
	STAD_TEMPORAL_CACHE,
	ReSTIR_GI_TEMPORAL_RESERVOIR,
	ReSTIR_GI_SPATIAL_RESERVOIR,
	COUNT
};

enum class Tonemapper
{
	NONE,
	ACES_FILMIC,
	UE4_FILMIC,
	NEUTRAL,
	COUNT
};

struct cbDisplayPass
{
	uint16_t DisplayOption;
	uint16_t Tonemapper;
	uint16_t VisualizeOcclusion;

	uint32_t InputDescHeapIdx;
	uint32_t ExposureDescHeapIdx;
	uint32_t IndirectDiffuseLiDescHeapIdx;
	uint32_t SVGFSpatialVarDescHeapIdx;
	uint32_t DenoiserTemporalCacheDescHeapIdx;
	uint32_t TemporalReservoir_A_DescHeapIdx;
	uint32_t TemporalReservoir_B_DescHeapIdx;
	uint32_t TemporalReservoir_C_DescHeapIdx;
	uint32_t SpatialReservoir_A_DescHeapIdx;
	uint32_t SpatialReservoir_B_DescHeapIdx;
	uint32_t SpatialReservoir_C_DescHeapIdx;
	uint32_t LUTDescHeapIdx;
};

#endif // FINAL_PASS_H
