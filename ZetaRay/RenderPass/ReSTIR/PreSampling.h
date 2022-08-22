#ifndef CANDIDATE_GENERATION_H
#define CANDIDATE_GENERATION_H

#include "../Common/HLSLCompat.h"

#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8
#define THREAD_GROUP_SIZE_Z 1

struct cbCandidateGeneration
{
	uint2 Resolution;
	uint OutputDescHeapOffset;
	uint NumSubsets;
	uint SubetSize;
};

#endif