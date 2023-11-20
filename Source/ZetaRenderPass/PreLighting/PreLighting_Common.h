#ifndef PRE_LIGHTING_COMMON_H
#define PRE_LIGHTING_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"
#include "../../ZetaCore/RayTracing/RtCommon.h"

#define ESTIMATE_TRI_LUMEN_GROUP_DIM_X 256u
#define ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI 64
#define ESTIMATE_TRI_LUMEN_WAVE_LEN 32
#define ESTIMATE_TRI_LUMEN_NUM_TRIS_PER_GROUP (ESTIMATE_TRI_LUMEN_GROUP_DIM_X / ESTIMATE_TRI_LUMEN_WAVE_LEN)

#define PRESAMPLE_EMISSIVE_GROUP_DIM_X 64u

#define ESTIMATE_CURVATURE_GROUP_DIM_X 8u
#define ESTIMATE_CURVATURE_GROUP_DIM_Y 8u

#define NUM_SAMPLES_PER_VOXEL 64

struct cbPresampling
{
	uint32_t NumTotalSamples;
};

struct cbCurvature
{
	uint32_t OutputUAVDescHeapIdx;
};

struct cbLVG
{
	float Extents_x;
	float Extents_y;
	float Extents_z;
	float Offset_y;
	uint32_t GridDim_x;
	uint32_t GridDim_y;
	uint32_t GridDim_z;
	uint32_t NumTotalSamples;
};

#endif