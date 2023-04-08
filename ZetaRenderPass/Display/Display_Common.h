#ifndef FINAL_PASS_H
#define FINAL_PASS_H

#include "../../ZetaCore/Core/HLSLCompat.h"

enum class DisplayOption
{
	DEFAULT,
	BASE_COLOR,
	NORMAL,
	METALNESS_ROUGHNESS,
	EMISSIVE,
	DEPTH,
	EXPOSURE_HEATMAP,
	DIFFUSE_DNSR,
	ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR,
	ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR,
	ReSTIR_GI_SPECULAR_TEMPORAL,
	ReSTIR_GI_SPECULAR_SPATIAL,
	SPECULAR_DNSR,
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
	uint16_t pad;

	uint32_t InputDescHeapIdx;
	uint32_t ExposureDescHeapIdx;
	uint32_t DiffuseDNSRTemporalCacheDescHeapIdx;
	uint32_t DiffuseTemporalReservoir_A_DescHeapIdx;
	uint32_t DiffuseTemporalReservoir_B_DescHeapIdx;
	uint32_t DiffuseSpatialReservoir_A_DescHeapIdx;
	uint32_t DiffuseSpatialReservoir_B_DescHeapIdx;
	uint32_t SpecularTemporalReservoir_A_DescHeapIdx;
	uint32_t SpecularTemporalReservoir_B_DescHeapIdx;
	uint32_t SpecularTemporalReservoir_D_DescHeapIdx;
	uint32_t SpecularSpatialReservoir_A_DescHeapIdx;
	uint32_t SpecularSpatialReservoir_B_DescHeapIdx;
	uint32_t SpecularSpatialReservoir_D_DescHeapIdx;
	uint32_t SpecularDNSRTemporalCacheDescHeapIdx;
	uint32_t LUTDescHeapIdx;
};

#endif // FINAL_PASS_H
