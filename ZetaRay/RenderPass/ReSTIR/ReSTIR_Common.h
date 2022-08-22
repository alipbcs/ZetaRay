#ifndef RESTIR_COMMON_H
#define RESTIR_COMMON_H

#include "../Common/HLSLCompat.h"
#include "../Common/LightSourceData.h"

#define ReSTIR_THREAD_GROUP_SIZE_X 8
#define ReSTIR_THREAD_GROUP_SIZE_Y 8
#define ReSTIR_THREAD_GROUP_SIZE_Z 1

struct cbReSTIR
{
	// Resources
	uint_ LinearDepthGradDescHeapIdx;
	// RWTexture2D<float4>
	uint_ OutputDescHeapIdx;

	uint_ NumRISCandidates;
	float MaxMScale;
	float NormalAngleThreshold;
	float DepthToleranceScale;
	float TemporalSampleBiasWeightThreshold;
	uint_ NumSpatialSamples;
	uint_ NumSpatialSamplesWhenTemporalReuseFailed;
	float SpatialNeighborSearchRadius;

	// Env. Map
	float OndDivPatchArea;	// Assuming uniform sampling, probability of any sample inside each path is 1 / Area(Patch)
	uint_ NumPatches;		// Number of patches	
	float dPhi;				// dPhi = TWO_PI / NUM_PATCHES_U;

	uint_ HaltonSeqLength;

	uint_ DispatchDimX;
	uint_ DispatchDimY;
	uint_ TileWidth;
	uint_ Log2TileWidth;
	uint_ NumGroupsInTile;
};

struct Reservoir
{
	float2_ EnvMapUV;
	float TargetFunction;
	float M;

	float WeightSum;
	float W;

	// temporal reservoir
	bool WasTemporalReservoirVisible;
	bool DidTemporalReuse;
};

#endif