#ifndef AUTO_EXPOSURE_H
#define AUTO_EXPOSURE_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define THREAD_GROUP_SIZE_HIST_X 16
#define THREAD_GROUP_SIZE_HIST_Y 16

struct cbAutoExposure
{
	uint32_t MipLevels;
	uint32_t NumThreadGroupsX;
	uint32_t NumThreadGroupsY;

	uint16_t Mip5DimX;
	uint16_t Mip5DimY;

	uint32_t InputDescHeapIdx;
	uint32_t OutputMip5DescHeapIdx;
	uint32_t OutputLastMipDescHeapIdx;

	float MinLum;
	float MaxLum;

	uint16_t ClampLum;
	uint16_t pad;
};

#endif // AUTO_EXPOSURE_H