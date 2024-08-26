#ifndef RESTIR_PT_PARAMS_H
#define RESTIR_PT_PARAMS_H

// Minimum number of bounces before performing Russian Roulette
#define MIN_NUM_BOUNCES_RUSSIAN_ROULETTE 3

// Probability of next event estimation with sun vs sky
#define P_SUN_VS_SKY 0.65

// When enabled, sun is treated as a disk area light with radius based on the 
// sun angular diameter, otherwise a directional light. Has minimal impact on 
// quality and performance (currently not supported for ReSTIR PT).
#define SUN_DISK_SAMPLING 0

// Suppresses fireflies from sun at the expense of some bias
#define SUPPRESS_SUN_FIREFLIES 1

// When true, uses less precise but faster-to-trace shadow rays
#define APPROXIMATE_EMISSIVE_SHADOW_RAY 1

// Some resampling parameters
#define MAX_PLANE_DIST_REUSE 0.005
#define MAX_ROUGHNESS_DIFF_REUSE 0.175f
#define MIN_NORMAL_SIMILARITY_SPATIAL_REUSE 0.5f  // ~60 degrees
#define MAX_ROUGHNESS_DIFF_SPATIAL_REUSE 0.1f

// w_sum growing rapidly is a strong sign of uncontrolled spatial correlations
// that cause unbounded weight growth.
#define MAX_W_SUM_DERIV_SPATIAL_REUSE 0.5f

#endif