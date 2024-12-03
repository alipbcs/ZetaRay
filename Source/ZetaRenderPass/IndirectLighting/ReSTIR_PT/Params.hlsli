#ifndef RESTIR_PT_PARAMS_H
#define RESTIR_PT_PARAMS_H

// Minimum number of bounces before performing Russian Roulette
#define MIN_NUM_BOUNCES_RUSSIAN_ROULETTE 3

// When enabled, sun is treated as a disk area light with radius based on the 
// sun angular diameter, otherwise a directional light. Has minimal impact on 
// quality and performance (currently not supported for ReSTIR PT).
#define SUN_DISK_SAMPLING 0

// When true, uses less precise but faster-to-trace shadow rays
#define APPROXIMATE_EMISSIVE_SHADOW_RAY 1

// Some resampling parameters
#define MAX_PLANE_DIST_REUSE 1
#define MAX_ROUGHNESS_DIFF_REUSE 0.175f
#define MIN_NORMAL_SIMILARITY_SPATIAL_REUSE 0.9f  // ~25 degrees
#define MAX_ROUGHNESS_DIFF_TEMPORAL_REUSE 0.3f
#define MAX_ROUGHNESS_DIFF_SPATIAL_REUSE 0.05f

// Use a lower M for less aggressive spatial reuse in following cases
#define M_MAX_X_K_TRANSMISSIVE 4
#define M_MAX_X_K_IN_MOTION 4

#define SKY_SAMPLING_PREFER_PERFORMANCE 1

#endif