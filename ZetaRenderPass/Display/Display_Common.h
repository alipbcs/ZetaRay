#ifndef FINAL_PASS_H
#define FINAL_PASS_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define DOF_CoC_THREAD_GROUP_DIM_X 8u
#define DOF_CoC_THREAD_GROUP_DIM_Y 8u

#define DOF_GATHER_THREAD_GROUP_DIM_X 8u
#define DOF_GATHER_THREAD_GROUP_DIM_Y 8u

#define GAUSSIAN_FILTER_THREAD_GROUP_DIM_X 8u
#define GAUSSIAN_FILTER_THREAD_GROUP_DIM_Y 8u

enum class DisplayOption
{
	DEFAULT,
	BASE_COLOR,
	NORMAL,
	METALNESS_ROUGHNESS,
	EMISSIVE,
	DEPTH,
	CURVATURE,
	EXPOSURE_HEATMAP,
	COUNT
};

enum class Tonemapper
{
	NONE,
	ACES_FITTED,
	NEUTRAL,
	AgX,
	COUNT
};

struct cbDisplayPass
{
	uint16_t DisplayOption;
	uint16_t Tonemapper;
	//uint16_t VisualizeOcclusion;
	uint16_t AutoExposure;
	uint16_t pad;

	uint32_t InputDescHeapIdx;
	uint32_t ExposureDescHeapIdx;
	uint32_t DiffuseDNSRTemporalCacheDescHeapIdx;
	uint32_t DiffuseTemporalReservoir_A_DescHeapIdx;
	uint32_t DiffuseTemporalReservoir_B_DescHeapIdx;
	uint32_t DiffuseSpatialReservoir_A_DescHeapIdx;
	uint32_t DiffuseSpatialReservoir_B_DescHeapIdx;
	uint32_t LUTDescHeapIdx;

	float Saturation;
};

struct cbDoF_Gather
{
	uint32_t CompositedSrvDescHeapIdx;
	uint32_t CoCSrvDescHeapIdx;
	uint32_t CoCUavDescHeapIdx;
	uint32_t OutputUavDescHeapIdx;

	float FocalLength;
	float FStop;
	float FocusDepth;

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

#endif // FINAL_PASS_H
