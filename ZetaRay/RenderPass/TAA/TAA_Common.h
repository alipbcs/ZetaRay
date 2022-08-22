#ifndef TAA_H
#define TAA_H

#include "../Common/HLSLCompat.h"

#define TAA_THREAD_GROUP_SIZE_X 8
#define TAA_THREAD_GROUP_SIZE_Y 8
#define TAA_THREAD_GROUP_SIZE_Z 1

struct cbTAA
{
	float BlendWeight;
	uint_ InputDescHeapIdx;
	uint_ PrevOutputDescHeapIdx;
	uint_ CurrOutputDescHeapIdx;
	uint_ TemporalIsValid;
	uint_ CatmullRomFiltering;
};

#endif