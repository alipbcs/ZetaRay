#ifndef RESTIR_GI_COMMON_H
#define RESTIR_GI_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define RGI_TEMPORAL_THREAD_GROUP_SIZE_X 16
#define RGI_TEMPORAL_THREAD_GROUP_SIZE_Y 16
#define RGI_TEMPORAL_THREAD_GROUP_SIZE_Z 1

#define RGI_SPATIAL_THREAD_GROUP_SIZE_X 32
#define RGI_SPATIAL_THREAD_GROUP_SIZE_Y 32
#define RGI_SPATIAL_THREAD_GROUP_SIZE_Z 1

#define RGI_TEMPORAL_TILE_WIDTH 16
#define RGI_TEMPORAL_LOG2_TILE_WIDTH 4

#define RGI_SPATIAL_TILE_WIDTH 16
#define RGI_SPATIAL_LOG2_TILE_WIDTH 4

struct cbTemporalPass
{
	uint32_t FrameCounter;
	uint32_t PrevTemporalReservoir_A_DescHeapIdx;
	uint32_t PrevTemporalReservoir_B_DescHeapIdx;
	uint32_t PrevTemporalReservoir_C_DescHeapIdx;

	uint32_t CurrTemporalReservoir_A_DescHeapIdx;
	uint32_t CurrTemporalReservoir_B_DescHeapIdx;
	uint32_t CurrTemporalReservoir_C_DescHeapIdx;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;

	uint16_t NumGroupsInTile;
	uint16_t IsTemporalReservoirValid;
	uint16_t DoTemporalResampling;
	uint16_t PdfCorrection;

	uint16_t SampleIndex;
	uint16_t CheckerboardTracing;
};

struct cbSpatialPass
{
	float NormalExp;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;

	uint32_t InputReservoir_A_DescHeapIdx;
	uint32_t InputReservoir_B_DescHeapIdx;
	uint32_t InputReservoir_C_DescHeapIdx;
	uint32_t OutputReservoir_A_DescHeapIdx;
	uint32_t OutputReservoir_B_DescHeapIdx;
	uint32_t OutputReservoir_C_DescHeapIdx;
	uint16_t NumGroupsInTile;
	uint16_t PdfCorrection;
	uint16_t IsFirstPass;
	uint16_t pad;
};

#endif
