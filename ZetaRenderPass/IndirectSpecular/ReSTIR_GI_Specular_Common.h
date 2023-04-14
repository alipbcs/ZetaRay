#ifndef RESTIR_GI_SPEC_COMMON_H
#define RESTIR_GI_SPEC_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define RGI_SPEC_TEMPORAL_GROUP_DIM_X 8
#define RGI_SPEC_TEMPORAL_GROUP_DIM_Y 8

#define RGI_SPEC_TEMPORAL_TILE_WIDTH 16
#define RGI_SPEC_TEMPORAL_LOG2_TILE_WIDTH 4

#define RGI_SPEC_SPATIAL_GROUP_DIM_X 32
#define RGI_SPEC_SPATIAL_GROUP_DIM_Y 32

#define RGI_SPEC_SPATIAL_TILE_WIDTH 16
#define RGI_SPEC_SPATIAL_LOG2_TILE_WIDTH 4

#define SPECULAR_DNSR_GROUP_DIM_X 8
#define SPECULAR_DNSR_GROUP_DIM_Y 8

#define ESTIMATE_CURVATURE_GROUP_DIM_X 8
#define ESTIMATE_CURVATURE_GROUP_DIM_Y 8

struct cb_RGI_Spec_Temporal
{
	uint32_t PrevTemporalReservoir_A_DescHeapIdx;
	uint32_t PrevTemporalReservoir_B_DescHeapIdx;
	uint32_t PrevTemporalReservoir_C_DescHeapIdx;
	uint32_t PrevTemporalReservoir_D_DescHeapIdx;

	uint32_t CurrTemporalReservoir_A_DescHeapIdx;
	uint32_t CurrTemporalReservoir_B_DescHeapIdx;
	uint32_t CurrTemporalReservoir_C_DescHeapIdx;
	uint32_t CurrTemporalReservoir_D_DescHeapIdx;
	
	uint32_t CurvatureSRVDescHeapIdx;

	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t IsTemporalReservoirValid;
	uint16_t DoTemporalResampling;
	uint16_t PdfCorrection;
	uint16_t SampleIndex;
	uint16_t CheckerboardTracing;
	uint16_t NumGroupsInTile;
	float RoughnessCutoff;
	uint32_t M_max;
	float MinRoughnessResample;
	float HitDistSigmaScale;
};

struct cb_RGI_Spec_Spatial
{
	//float NormalExp;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t NumIterations;
	uint16_t M_max;
	uint16_t Radius;
	uint16_t PdfCorrection;
	float RoughnessCutoff;
	float HitDistSigmaScale;
	float MinRoughnessResample;

	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;
	uint32_t InputReservoir_C_DescHeapIdx;
	uint32_t InputReservoir_D_DescHeapIdx;

	uint32_t OutputReservoir_A_DescHeapIdx;
	uint32_t OutputReservoir_B_DescHeapIdx;
	uint32_t OutputReservoir_D_DescHeapIdx;

	uint16_t NumGroupsInTile;
	uint16_t DoSpatialResampling;
};

struct cbDNSR
{
	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;
	uint32_t InputReservoir_D_DescHeapIdx;

	uint32_t PrevTemporalCacheDescHeapIdx;
	uint32_t CurrTemporalCacheDescHeapIdx;
	uint32_t CurvatureSRVDescHeapIdx;
	
	float RoughnessCutoff;
	float ViewAngleExp;
	float RoughnessExpScale;
	uint16_t Denoise;
	uint16_t IsTemporalCacheValid;
	uint16_t MaxTSPP;
	uint16_t pad;
};

struct cbCurvature
{
	uint32_t OutputUAVDescHeapIdx;
};

#endif
