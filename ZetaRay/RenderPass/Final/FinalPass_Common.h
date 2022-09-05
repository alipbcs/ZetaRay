#ifndef FINAL_PASS_H
#define FINAL_PASS_H

#include "../Common/HLSLCompat.h"

struct cbFinalPass
{
	// Texture2D<float4>
	uint_ InputDescHeapIdx;
	//float KeyValue;

	uint_ DisplayDepth;
	uint_ DisplayNormals;
	uint_ DisplayBaseColor;
	uint_ DisplayMetalnessRoughness;
	uint_ DisplayMotionVec;
	uint_ DisplayIndirectDiffuse;
	uint_ DisplayStadTemporalCache;
	uint_ DoTonemapping;
	uint_ VisualizeOcclusion;

	uint_ IndirectDiffuseLiDescHeapIdx;
	uint_ SVGFSpatialVarDescHeapIdx;
	uint_ DenoiserTemporalCacheDescHeapIdx;
};

#endif // FINAL_PASS_H
