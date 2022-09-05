#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../Common/HLSLCompat.h"

#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8
#define THREAD_GROUP_SIZE_Z 1

struct cbCompositing
{
	// 
	// Resources
	//
	
	// Texture2D<float4>
	uint_ HDRLightAccumDescHeapIdx;

	// current frame's integrated values
	// Texture2D<uint3>: (tspp | color.r >> 16, color.g | color.b >> 16, lum | lum^2 >> 16)
	uint_ DenoisedLindDescHeapIdx;

	// Texture2D<half4>
//	uint_ IndirectLiRayTDescHeapIdx;

	// Texture3D<half4>
	uint_ InscatteringDescHeapIdx;

	uint_ StadDenoiser;

	uint_ AccumulateInscattering;
	float DepthMappingExp;
	float VoxelGridNearZ;
	float VoxelGridFarZ;
};

#endif // COMPOSITING_H