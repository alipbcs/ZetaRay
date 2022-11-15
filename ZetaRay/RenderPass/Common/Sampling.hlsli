#ifndef SAMPLING_H
#define SAMPLING_H

#include "Common.hlsli"

//--------------------------------------------------------------------------------------
// RNG
//
// Refs: 
// https://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
//--------------------------------------------------------------------------------------

struct RNG
{
	static uint PcgHash(uint x)
	{
		uint state = x * 747796405u + 2891336453u;
		uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}
	
	// A seperate pcg PRNG instance for each thread or pixel, seeded with unique values
	static RNG Init(uint2 pixel, uint frame, uint2 resolution)
	{
//		uint rngState = dot(pixel, uint2(1, resolutionX)) ^ pcg_hash(frame);
		RNG rng;
		
		rng.State = dot(pixel, resolution) ^ PcgHash(frame);
		rng.State = rng.PcgHash(rng.State);
		
		return rng;
	}
	
	// for following samples after initial sample
	uint RandPcg()
	{
		State = State * 747796405u + 2891336453u;
		uint word = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
		
		return (word >> 22u) ^ word;
	}

	// 32-bit floating point (https://en.wikipedia.org/wiki/Single-precision_floating-point_format)
	//  31 | 30 ..... 23 | 22 ...... 0
	// sign    exponent      fraction
	// 9 high-order bits that correspond to sign and exponent are set to 0 and 127 respectively
	// 23 low-order fraction bits come from a random integer
	float RandUniform()
	{
		uint x = RandPcg();
		
		// [1, 2) -> [0, 1)
		return asfloat(0x3f800000 | (x >> 9)) - 1.0f;
	}
			
	// random uint
	uint RandUintRange(uint lower, uint upper)
	{
		uint x = RandPcg();
		return lower + uint(RandUniform() * float(upper - lower + 1));
	}
	
	float2 RandUniform2D()
	{
		float u0 = RandUniform();
		float u1 = RandUniform();
		
		return float2(u0, u1);
	}
	
	uint State;
};
	
//--------------------------------------------------------------------------------------
// Sampling Transformations
//
//	Refs:
//	1. Physically Based Rendering 3rd Ed.
//	2. Ray Tracing Gems 1, Chapter 16
//--------------------------------------------------------------------------------------

namespace Sampling
{
	// Returns samples about the (0, 1, 0) axis
	float3 UniformSampleHemisphere(in float2 u, out float pdf)
	{
		const float phi = TWO_PI * u.y;
		const float sinTheta = sqrt(1.0f - u.x * u.x);
	
		const float x = cos(phi) * sinTheta;
		const float y = u.x;
		const float z = sin(phi) * sinTheta;
	
		// w.r.t. solid angle
		pdf = ONE_DIV_TWO_PI;
		
		return float3(x, y, z);
	}

	// Returns samples about the (0, 1, 0) axis
	float3 SampleCosineWeightedHemisphere(in float2 u, out float pdf)
	{
		const float phi = TWO_PI * u.y;
		const float sinTheta = sqrt(u.x);

		const float x = cos(phi) * sinTheta;
		const float y = sqrt(1.0f - u.x); // == cos(theta)
		const float z = sin(phi) * sinTheta;
	
		// w.r.t. solid angle
		pdf = y * ONE_DIV_PI;
	
		return float3(x, y, z);
	}

	//--------------------------------------------------------------------------------------
	// Sampling Shapes
	//--------------------------------------------------------------------------------------

	// Returns samples about the (0, 1, 0) axis
	float3 UniformSampleCone(in float2 u, in float cosThetaMax, out float pdf)
	{
		const float phi = TWO_PI * u.y;

		const float cosTheta = 1.0f - u.x + u.x * cosThetaMax;
		const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

		// x = sin(theta) * cos(phi)
		// y = cos(theta)
		// z = sin(theta) * sin(phi)
		const float x = cos(phi) * sinTheta;
		const float y = cosTheta;
		const float z = sin(phi) * sinTheta;

		// w.r.t. solid angle
		pdf = ONE_DIV_TWO_PI * rcp(1.0f - cosThetaMax);
	
		return float3(x, y, z);
	}

	// Area = PI;
	float2 UniformSampleDisk(in float2 u)
	{
		const float r = sqrt(u.x);
		const float phi = TWO_PI * u.y;
	
		return float2(r * cos(phi), r * sin(phi));
	}

	// Area = PI;
	float2 UniformSampleDiskConcentricMapping(in float2 u)
	{
		float a = 2.0f * u.x - 1.0f;
		float b = 2.0f * u.y - 1.0f;

		float r;
		float phi;
	
		if (a * a > b * b)
		{
			r = a;
			phi = PI_DIV_4 * (b / a);
		}
		else
		{
			r = b;
			phi = PI_DIV_2 - PI_DIV_4 * (a / b);
		}
	
		return float2(r * cos(phi), r * sin(phi));
	}

	// Area = FOUR_PI;
	float3 UniformSampleSphere(in float2 u)
	{
		// Compute radius r (branchless).
		float u0 = 2.0f * u.x - 1.0f;
		float u1 = 2.0f * u.y - 1.0f;

		float d = 1.0f - (abs(u0) + abs(u1));
		float r = 1.0f - abs(d);

		// Compute phi in the first quadrant (branchless, except for the
		// division-by-zero test), using sign(u) to map the result to the
		// correct quadrant below.
		float phi = (r == 0) ? 0 : PI_DIV_4 * ((abs(u1) - abs(u0)) / r + 1.0f);
		float f = r * sqrt(2.0f - r * r);
		float x = f * sign(u0) * cos(phi);
		float y = f * sign(u1) * sin(phi);
		float z = sign(d) * (1.0f - r * r);
	
		return float3(x, y, z);
	}

	// Area = 0.5f * abs(cross(v1 - v0, v2 - v0))
	float3 UniformSampleTriangle(float2 u)
	{
		float b0 = 1.0 - sqrt(u.x);
		float b1 = (1.0 - b0) * u.y;
		float b2 = 1.0 - b0 - b1;
	
		return float3(b0, b1, b2);
	}

	//--------------------------------------------------------------------------------------
	// Blue noise sampler
	//
	// Ref: "A Low-Discrepancy Sampler that Distributes Monte Carlo Errors as a Blue Noise in Screen Space"
	//--------------------------------------------------------------------------------------

	// This is for 32 samples per-pixel
	// Sample index: frame number % 32
	// Sample dimension: (0, 1) for the indirect samples and more for additional dimensions
	float samplerBlueNoiseErrorDistribution(StructuredBuffer<uint> g_owenScrambledSobolSeq,
		StructuredBuffer<uint> g_rankingTile, StructuredBuffer<uint> g_scramblingTile,
		int pixel_i, int pixel_j, int sampleIndex, int sampleDimension)
	{
		// wrap arguments
		pixel_i = pixel_i & 127;
		pixel_j = pixel_j & 127;
		sampleIndex = sampleIndex & 255;
		sampleDimension = sampleDimension & 255;

		// xor index based on optimized ranking
		int rankedSampleIndex = sampleIndex ^ g_rankingTile[sampleDimension + (pixel_i + pixel_j * 128) * 8];

		// fetch value in sequence
		int value = g_owenScrambledSobolSeq[sampleDimension + rankedSampleIndex * 256];

		// If the dimension is optimized, xor sequence value based on optimized scrambling
		value = value ^ g_scramblingTile[(sampleDimension & 7) + (pixel_i + pixel_j * 128) * 8];

		// convert to float and return
		float v = (0.5f + value) / 256.0f;
	
		return v;
	}
}

#endif