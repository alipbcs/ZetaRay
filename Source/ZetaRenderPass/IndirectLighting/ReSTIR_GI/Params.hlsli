#ifndef RESTIR_GI_PARAMS_H
#define RESTIR_GI_PARAMS_H

// Minimum number of bounces before performing Russian Roulette
#define MIN_NUM_BOUNCES_RUSSIAN_ROULETTE 3

// Probability of next event estimation with sun vs sky
#define P_SUN_VS_SKY 0.65

// Use multiple importance sampling for direct lighting. Reduces noise (especially for 
// smoother surface) with a moderate performance cost.
#define USE_MIS 1

// Number of light samples for area sampling in NEE. Higher (up to four)
// reduces noise but with a significant performance cost.
#define NEE_NUM_LIGHT_SAMPLES 1

// When disabled, direct lighting uses MIS just for the first indirect hit 
// and power sampling for the subsequent hits. Improves performance at the 
// expense of quality.
#define MIS_ALL_BOUNCES 0
#define MIS_NUM_LIGHT_SAMPLES 1
#define MIS_NON_DIFFUSE_BSDF_SAMPLING 1

// When enabled, sun is treated as a disk area light with radius based on the 
// sun angular diameter, otherwise a directional light. Has minimal impact on 
// quality and performance.
#define SUN_DISK_SAMPLING 0

// Suppresses fireflies from sun at the expense of some bias
#define SUPPRESS_SUN_FIREFLIES 1

// When true, uses less precise but faster-to-trace shadow rays
#define APPROXIMATE_EMISSIVE_SHADOW_RAY 1

// Maximum roughness to use virtual motion instead of surface motion for temporal resampling
#define MAX_ROUGHNESS_VIRTUAL_MOTION 0.075

// Some resampling parameters
#define MAX_PLANE_DIST_REUSE 0.005
#define MIN_NORMAL_SIMILARITY_REUSE_TEMPORAL 0.906307787    // within 15 degrees
#define MAX_ROUGHNESS_DIFF_REUSE 0.05f
#define NUM_TEMPORAL_SEARCH_ITER 3
#define TEMPORAL_SEARCH_RADIUS 16
#define MIN_NUM_SPATIAL_SAMPLES 1
#define SPATIAL_SEARCH_RADIUS 32

#endif