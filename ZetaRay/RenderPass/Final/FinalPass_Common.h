#ifndef FINAL_PASS_H
#define FINAL_PASS_H

#include "../Common/HLSLCompat.h"

struct cbFinalPass
{
	// Texture2D<float4>
	uint32_t InputDescHeapIdx;
	//float KeyValue;

	uint16_t DisplayDepth;
	uint16_t DisplayNormal;
	uint16_t DisplayBaseColor;
	uint16_t DisplayMetalnessRoughness;
	uint16_t DisplayStadTemporalCache;
	uint16_t DisplayReSTIR_GI_TemporalReservoir;
	uint16_t DisplayReSTIR_GI_SpatialReservoir;
	uint16_t DoTonemapping;
	uint16_t VisualizeOcclusion;

	uint32_t IndirectDiffuseLiDescHeapIdx;
	uint32_t SVGFSpatialVarDescHeapIdx;
	uint32_t DenoiserTemporalCacheDescHeapIdx;
	uint32_t TemporalReservoir_A_DescHeapIdx;
	uint32_t TemporalReservoir_B_DescHeapIdx;
	uint32_t TemporalReservoir_C_DescHeapIdx;
	uint32_t SpatialReservoir_A_DescHeapIdx;
	uint32_t SpatialReservoir_B_DescHeapIdx;
	uint32_t SpatialReservoir_C_DescHeapIdx;
};

#endif // FINAL_PASS_H
