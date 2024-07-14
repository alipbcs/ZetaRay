#define USE_PRESAMPLED_SETS
#define USE_LVG
// A current limitation is lack of a way to effectively choose between emissive meshes and sun 
// or sky for NEE. Effective importance sampling requires visibility information at the shading 
// point (e.g. a room where the sun can't reach). For now, just manually pick between them.
#define NEE_EMISSIVE
#include "ReSTIR_GI.hlsl"