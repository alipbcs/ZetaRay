#ifndef RESTIR_DI_PARAMS_H
#define RESTIR_DI_PARAMS_H

#define NUM_LIGHT_CANDIDATES 3
#define MIN_NUM_SPATIAL_SAMPLES 1
#define TEMPORAL_SEARCH_RADIUS 8
#define SPATIAL_SEARCH_RADIUS 32
#define NUM_TEMPORAL_SEARCH_ITER 4
#define MAX_NUM_BRDF_SAMPLES 2
#define APPROXIMATE_EMISSIVE_SHADOW_RAY 1

// heuristics for deciding when to reuse between different distributions (temporal or spatial neighbors)
#define MAX_PLANE_DIST_REUSE 0.005
#define MIN_NORMAL_SIMILARITY_REUSE 0.906307787    // within 25 degrees
#define MAX_ROUGHNESS_DIFF_REUSE 0.15f

#endif