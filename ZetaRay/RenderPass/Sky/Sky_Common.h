#ifndef SKY_H
#define SKY_H

#include "../Common/HLSLCompat.h"

#define SKY_VIEW_LUT_THREAD_GROUP_SIZE_X 8
#define SKY_VIEW_LUT_THREAD_GROUP_SIZE_Y 8
#define SKY_VIEW_LUT_THREAD_GROUP_SIZE_Z 1

#define INSCATTERING_THREAD_GROUP_SIZE_X 128
#define INSCATTERING_THREAD_GROUP_SIZE_Y 1
#define INSCATTERING_THREAD_GROUP_SIZE_Z 1

struct cbSky
{
	uint32_t LutWidth;
	uint32_t LutHeight;

	uint32_t NumVoxelsX;
	uint32_t NumVoxelsY;

	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;

	// 
	// Resources
	//

	// RWTexture3D<half4>
	// RWTexture2D<float4>
	uint32_t LutDescHeapIdx;
	uint32_t VoxelGridDescHeapIdx;
};

#endif