#ifndef FINAL_PASS_H
#define FINAL_PASS_H

#include "../Common/HLSLCompat.h"

struct cbFinalPass
{
	// Texture2D<float4>
	uint32_t InputDescHeapIdx;
	//float KeyValue;

	uint32_t DisplayDepth;
	uint32_t DisplayNormals;
	uint32_t DisplayBaseColor;
	uint32_t DisplayMetalnessRoughness;
	uint32_t DisplayMotionVec;
	uint32_t DisplayIndirectDiffuse;
	uint32_t DisplayStadTemporalCache;
	uint32_t DoTonemapping;
	uint32_t VisualizeOcclusion;

	uint32_t IndirectDiffuseLiDescHeapIdx;
	uint32_t SVGFSpatialVarDescHeapIdx;
	uint32_t DenoiserTemporalCacheDescHeapIdx;
};

#endif // FINAL_PASS_H
