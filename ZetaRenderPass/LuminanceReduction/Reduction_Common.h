#ifndef REDUCTION_H
#define REDUCTION_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define THREAD_GROUP_SIZE_X_FIRST 32
#define THREAD_GROUP_SIZE_Y_FIRST 32
#define THREAD_GROUP_SIZE_Z_FIRST 1

struct cbReduction
{
	//
	// First Pass
	//

	// Texture2D<float4>
	uint32_t InputDescHeapIdx;
	uint32_t DispatchDimXFirstPass;

	//
	// Second Pass
	//

	uint32_t NumGroupsInFirstPass;		// e.g. 1920 * 1080 / (THREAD_GROUP_SIZE_X_FIRST * THREAD_GROUP_SIZE_Y_FIRST)

	// Using one thread block of size 1024, finish the sum
	// depending on resolution, there were n = W * H / (32 * 32) thread-groups in the first pass
	// where 32x32 is the thread group dim
	// That means reduction n numbers. To make sure 1024 threads can do the reduction have
	// each thread do NumToProcessPerThread sums
	uint32_t NumToProcessPerThreadSecondPass;	// ceil(NumGroupsInFirstPass * 1024) / 1024
};

#define THREAD_GROUP_SIZE_X_SECOND 1024
#define THREAD_GROUP_SIZE_Y_SECOND 1
#define THREAD_GROUP_SIZE_Z_SECOND 1

#endif // REDUCTION_H