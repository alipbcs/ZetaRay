#ifndef AUTO_EXPOSURE_H
#define AUTO_EXPOSURE_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define THREAD_GROUP_SIZE_HIST_X 16u
#define THREAD_GROUP_SIZE_HIST_Y 16u

#define HIST_BIN_COUNT 256

struct cbAutoExposureHist
{
	uint32_t InputDescHeapIdx;
	uint32_t ExposureDescHeapIdx;
	float MinLum;
	float LumRange;
	float LumMapExp;
	float AdaptationRate;
	float LowerPercentile;
	float UpperPercentile;
};

#endif // AUTO_EXPOSURE_H