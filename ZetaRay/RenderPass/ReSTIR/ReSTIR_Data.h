#ifndef ReSTIR_DATA_H
#define ReSTIR_DATA_H




//static const uint_ NumSubsets = 128;
//static const uint_ SubsetSize = 512;

//struct Reservoir
//{
//#ifdef __cplusplus
//	Reservoir()
//		: M(0),
//		WeightSum(0.0f),
//		W(0.0f),
//		LightSurfacePos(float3_(-1.0f, -1.0f, -1.0f)),
//		Barry(0),
//		y(-1)
//	{}
//#endif // __cplusplus
//
//	// Number of samples resampled
//	uint_ M;
//	
//	// Sum of resampled weights where weight(sample) = targetFunc(sample) / sourcePDF(sample)
//	float WeightSum;
//	
//	// Sample picked using the samples seen so far
//	uint_ y;							// light source index
//	float3_ LightSurfacePos;			// light source surface position
//
//#ifndef __cplusplus
//	half2 Barry;					// barrycentric coords for triangle light sources
//#else
//	uint32_t Barry;
//#endif
//	// Cache for expression 1 / targetFunc(y) * 1 / M * sigma_i^M (targetFunc(sample) / sourcePDF(sample))
//	// 1 / W is essentially an estimate for f / int(f) where f is the integrand:
//	//		f = L_e * f * cos(theta) where visibility is accounted for in L_e
//	float W;
//
//	// Cached values
//	float3_ CachedPixelCanidateBRDFxLe;
//	float3_ CachedTemporalCanidateBRDFxLe;
//	float3_ CachedTemporalCanidateWi;
//	float CachedTemporalCandidateT;
////	float3 CachedSpatialCanidateBRDFxLe;
//};

//struct Candidate
//{
	// Light from global light list
//	LightSource Light;

	// Light Probability according to light power distribution
//	float Prob;
//};

//static const uint_ INVALID_INDEX = -1;

#endif