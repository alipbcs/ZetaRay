#ifndef SVGF_COMMON_H
#define SVGF_COMMON_H

#include "../Common/HLSLCompat.h"

#define TEMPORAL_FILTER_THREAD_GROUP_SIZE_X 8
#define TEMPORAL_FILTER_THREAD_GROUP_SIZE_Y 8
#define TEMPORAL_FILTER_THREAD_GROUP_SIZE_Z 1

#define SPATIAL_VAR_THREAD_GROUP_SIZE_X 8
#define SPATIAL_VAR_THREAD_GROUP_SIZE_Y 8
#define SPATIAL_VAR_THREAD_GROUP_SIZE_Z 1

#define GAUSSAIN_FILT_THREAD_GROUP_SIZE_X 8
#define GAUSSAIN_FILT_THREAD_GROUP_SIZE_Y 8
#define GAUSSAIN_FILT_THREAD_GROUP_SIZE_Z 1

#define WAVELET_TRANSFORM_THREAD_GROUP_SIZE_X 8
#define WAVELET_TRANSFORM_THREAD_GROUP_SIZE_Y 8
#define WAVELET_TRANSFORM_THREAD_GROUP_SIZE_Z 1

struct cbTemporalFilter
{
	uint_ MaxTspp;
	bool ClampHistory;
	uint_ MinTsppToUseTemporalVar;
	float MinConsistentWeight;	// 1e-3

	float BilinearNormalScale;
	float BilinearNormalExp;
	float BilinearGeometryMaxPlaneDist;

	float ClampingMinStd;
	float ClampingStdScale;
	float ClampingTsppAdjustmentScaleByDifference;
	float MinLumVariance;

	//
	// Resources
	//

	// Texture2D<float2>
	uint_ LinearDepthGradDescHeapIdx;

	// Incoming indirect light L_i for each surface position (element w is rayhitdist) in
	// the gbuffer. Note that is indirect light being received by the surface pos rather than the 
	// indirect light reflected back towards the viewer (due to indirect illumination L_i). Reason 
	// for doing so is that shading involves texture details that make denoising more difficult. Therfore,
	// shading is performed after denoising (here shading is just multiplication by Lambert's BRDF)
	// Texture2D<half4>
	uint_ IndirectLiRayTDescHeapIdx;

	// temporal cache
	// Texture2D<uint3>: (tspp | color.r >> 16, color.g | color.b >> 16, lum | lum^2 >> 16)
	uint_ PrevTemporalCacheDescHeapIdx;

	// next frame's temporal cache and current frame's integrated values
	// RWTexture2D<uint3>: (tspp | color.r >> 16, color.g | color.b >> 16, lum | lum^2 >> 16)
	uint_ CurrTemporalCacheDescHeapIdx;

	// spatial estimate of mean and variance of luminance
	// RWTexture2D<uint4>: (colMean, colVar, lumVar);
	uint_ SpatialLumVarDescHeapIdx;

	// maybe enable in the future to experiment with temporal cache <-> recent frame tradeoffs
//	float MinAccumulationSpeed;

	uint_ IsTemporalCacheValid;
};

struct cbSpatialVar
{
	int Radius;

	// Incoming indirect light L_i for each surface position (element w is rayhitdist) in
	// the gbuffer. Note that is indirect light being received by the surface pos rather than the 
	// indirect light reflected back towards the viewer (due to indirect illumination L_i). Reason 
	// for doing so is that shading involves texture details that make denoising more difficult. Therfore,
	// shading is performed after denoising (here shading is just multiplication by Lambert's BRDF)
	uint_ IndirectLiRayTDescHeapIdx;

	// RWTexture2D<half> 
	uint_ SpatialLumVarDescHeapIdx;
};

struct cbAtrousWaveletFilter
{
	float DepthWeightCutoff;
	float DepthSigma;
	float NormalSigma;
	float LumSigma;
	float MinVarianceToFilter;

	uint_ Step;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t TileWidth;		// must be a power of 2
	uint16_t Log2TileWidth;
	uint16_t NumGroupsInTile;  // == TileWidth * DispatchDimY

	uint_ IntegratedTemporalCacheDescHeapIdx;
	uint_ LumVarianceDescHeapIdx;
	// Texture2D<float2>
	uint_ LinearDepthGradDescHeapIdx;
};

struct cbGaussianFilter
{
	// RWTexture2D<half>
	uint_ SpatialLumVarDescHeapIdx;
	// RWTexture2D<half> 
	uint_ SpatialLumVarFilteredDescHeapIdx;
};

#ifndef __cplusplus

#if defined(FILTER_5x5)
// Filter is based on B_Spline interpolation
static const int16_t Radius = 2;
static const int16_t KernelWidth = 2 * Radius + 1;
static const float Kernel1D[2 * Radius + 1] = { 1.0f / 16, 1.0f / 4, 3.0f / 8, 1.0f / 4, 1.0f / 16 };
static const float Kernel2D[2 * Radius + 1][2 * Radius + 1] =
{
	Kernel1D[0] * Kernel1D[0], Kernel1D[0] * Kernel1D[1], Kernel1D[0] * Kernel1D[2], Kernel1D[0] * Kernel1D[3], Kernel1D[0] * Kernel1D[4],
	Kernel1D[1] * Kernel1D[0], Kernel1D[1] * Kernel1D[1], Kernel1D[1] * Kernel1D[2], Kernel1D[1] * Kernel1D[3], Kernel1D[1] * Kernel1D[4],
	Kernel1D[2] * Kernel1D[0], Kernel1D[2] * Kernel1D[1], Kernel1D[2] * Kernel1D[2], Kernel1D[2] * Kernel1D[3], Kernel1D[2] * Kernel1D[4],
	Kernel1D[3] * Kernel1D[0], Kernel1D[3] * Kernel1D[1], Kernel1D[3] * Kernel1D[2], Kernel1D[3] * Kernel1D[3], Kernel1D[3] * Kernel1D[4],
	Kernel1D[4] * Kernel1D[0], Kernel1D[4] * Kernel1D[1], Kernel1D[4] * Kernel1D[2], Kernel1D[4] * Kernel1D[3], Kernel1D[4] * Kernel1D[4],
};
#elif defined(FILTER_3x3)
// Gaussian Filter
static const int16_t Radius = 1;
static const int16_t KernelWidth = 2 * Radius + 1;
static const float Kernel1D[2 * Radius + 1] = { 0.27901, 0.44198, 0.27901 };
//static const float Kernel1D[2 * Radius + 1] = { 0.3125f, 0.375f, 0.3125f };
static const float Kernel2D[2 * Radius + 1][2 * Radius + 1] =
{
	{Kernel1D[0] * Kernel1D[0], Kernel1D[0] * Kernel1D[1], Kernel1D[0] * Kernel1D[2]},
	{Kernel1D[1] * Kernel1D[0], Kernel1D[1] * Kernel1D[1], Kernel1D[1] * Kernel1D[2]},
	{Kernel1D[2] * Kernel1D[0], Kernel1D[2] * Kernel1D[1], Kernel1D[2] * Kernel1D[2]}
};
#endif

#endif // __cplusplus

#endif
