#ifndef GAUSSIAN_FILTER
#define GAUSSIAN_FILTER

#include "../Common/HLSLCompat.h"

struct cbGaussianFilter
{
	uint_ InputDescHeapIdx;
	uint_ OutputDescHeapIdx;
	float InputWidth;
	float InputHeight;
};

#define GAUSSAIN_FILT_THREAD_GROUP_SIZE_X 8
#define GAUSSAIN_FILT_THREAD_GROUP_SIZE_Y 8
#define GAUSSAIN_FILT_THREAD_GROUP_SIZE_Z 1

//#ifndef __cplusplus

//// 3x3 Seperable Gaussian Filter
//#ifdef GAUSSIAN_3x3
//static const int Radius = 1;
//static const int KernelWidth = 2 * Radius + 1;
//static const float Kernel1D[2 * Radius + 1] = { 0.27901f, 0.44198f, 0.27901f };
//
//#elif defined GAUSSIAN_5x5
//static const int Radius = 2;
//static const int KernelWidth = 2 * Radius + 1;
//static const float Kernel1D[2 * Radius + 1] = { 0.0545f, 0.2442f, 0.4026f, 0.2442f, 0.0545f };

//#endif
//#endif // __cplusplus


#endif // GAUSSIAN_FILTER