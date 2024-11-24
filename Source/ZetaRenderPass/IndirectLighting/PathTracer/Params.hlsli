#ifndef PATH_TRACER_PARAMS_H
#define PATH_TRACER_PARAMS_H

// Minimum number of bounces before performing Russian Roulette
#define MIN_NUM_BOUNCES_RUSSIAN_ROULETTE 3

// Probability of next event estimation with sun vs sky
#define P_SUN_VS_SKY 0.65

// Use multiple importance sampling for direct lighting. Reduces noise (especially for 
// smoother surface) with a moderate performance cost.
#define USE_MIS 1

// When disabled, direct lighting uses MIS just for the first indirect hit and 
// power sampling for the subsequent hits. Improves performance at the expense 
// of quality.
#define MIS_ALL_BOUNCES 1
#define MIS_NUM_LIGHT_SAMPLES 1
#define MIS_NON_DIFFUSE_BSDF_SAMPLING 0

// When enabled, sun is treated as a disk area light with radius based on the 
// sun angular diameter, otherwise a directional light. Has minimal impact on 
// quality and performance.
#define SUN_DISK_SAMPLING 0

// When true, uses less precise but faster-to-trace shadow rays
#define APPROXIMATE_EMISSIVE_SHADOW_RAY 0

// When true, account for transmittance of rays travelling in homogeneous mediums
// (Currently limited to interior of translucent objects)
#define ACCOUNT_FOR_TRANSMITTANCE 1

#endif