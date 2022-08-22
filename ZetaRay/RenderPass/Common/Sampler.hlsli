#ifndef SAMPLER_H
#define SAMPLER_H

// Refs: 
// https://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
//
// Create an independent instance of the PRNG for each work item—vertex, pixel, 
// or compute-shader thread. Then we just need to seed them with different values, e.g. 
// using the vertex index, pixel screen coordinates, or thread index, and we’ll get 
// different sequences
uint PcgHash(uint x)
{
	uint state = x * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}
	
// Seed the PRGN using pcg
// Include pixel pos and frame number to ensure good distribution spatially and temporally
uint InitRNG(uint2 pixel, uint frame, uint2 resolution)
{
//		uint rngState = dot(pixel, uint2(1, resolutionX)) ^ pcg_hash(frame);
	uint rngState = dot(pixel, resolution) ^ PcgHash(frame);
	return PcgHash(rngState);
}
	
// for following samples after initial sample
uint RandPcg(inout uint rngState)
{
	rngState = rngState * 747796405u + 2891336453u;
	uint word = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
		
	return (word >> 22u) ^ word;
}

// 32-bit floating point (https://en.wikipedia.org/wiki/Single-precision_floating-point_format)
//  31  30 ..... 23  22 ...... 0
// sign  exponent      fraction
// 9 high-order bits that correspond to sign and exponent are set to 0 and 127 respectively
// 23 low-order fraction bits come from a random integer
float RandUniform(inout uint rngState)
{
	uint x = RandPcg(rngState);
		
	// [1, 2) -> [0, 1)
	return asfloat(0x3f800000 | (x >> 9)) - 1.0f;
}
		
// random uint
uint RandUintRange(inout uint rngState, uint lower, uint upper)
{
	uint x = RandPcg(rngState);
	return lower + uint(RandUniform(x) * float(upper - lower + 1));
}
	
float2 RandUniform2D(inout uint rngState)
{
	float u0 = RandUniform(rngState);
	float u1 = RandUniform(rngState);
		
	return float2(u0, u1);
}	


#endif