#ifndef SAMPLING_H
#define SAMPLING_H

#include "Math.hlsli"

//--------------------------------------------------------------------------------------
// Refs: 
// https://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
//--------------------------------------------------------------------------------------

struct RNG
{
	static uint Pcg(uint x)
	{
		uint state = x * 747796405u + 2891336453u;
		uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}

	// Ref: M. Jarzynski and M. Olano, "Hash Functions for GPU Rendering," Journal of Computer Graphics Techniques, 2020.
	static uint3 Pcg3d(uint3 v)
	{
		v = v * 1664525u + 1013904223u;
		v.x += v.y * v.z; 
		v.y += v.z * v.x; 
		v.z += v.x * v.y;
		v ^= v >> 16u;
		v.x += v.y * v.z; 
		v.y += v.z * v.x; 
		v.z += v.x * v.y;
		return v;
	}

	// A seperate pcg PRNG instance for each thread or pixel, seeded with unique values
	static RNG Init(uint2 pixel, uint frame)
	{
		RNG rng;		
#if 0
		rng.State = RNG::Pcg(pixel.x + RNG::Pcg(pixel.y + RNG::Pcg(frame)));
#else
		rng.State = RNG::Pcg3d(uint3(pixel, frame)).x;
#endif
		
		return rng;
	}

	static RNG Init(uint idx, uint frame)
	{
		RNG rng;
		rng.State = rng.Pcg(idx + Pcg(frame));
		
		return rng;
	}

	// for following samples after initial sample
	uint Next()
	{
		State = State * 747796405u + 2891336453u;
		uint word = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
		
		return (word >> 22u) ^ word;
	}

	float Uniform()
	{
		const float oneSubEps = 0x1.fffffep-1;
		const float uniformFloat01Inclusive = Next() * 0x1p-32f;    
		return uniformFloat01Inclusive < oneSubEps ? uniformFloat01Inclusive : oneSubEps;
	}

	// returns samples in [lower, upper)
	uint UintRange(uint lower, uint upper)
	{
		// TODO following is biased: https://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle
		return lower + uint(Uniform() * float(upper - lower));
	}

	float2 Uniform2D()
	{
		float u0 = Uniform();
		float u1 = Uniform();

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
	// Returns samples about the (0, 0, 1) axis
	float3 UniformSampleHemisphere(float2 u, out float pdf)
	{
		const float phi = TWO_PI * u.y;
		const float sinTheta = sqrt(1.0f - u.x * u.x);

		const float x = cos(phi) * sinTheta;
		const float y = sin(phi) * sinTheta;
		const float z = u.x;

		pdf = ONE_OVER_2_PI;

		return float3(x, y, z);
	}

	// Returns samples about the (0, 0, 1) axis
	float3 SampleCosineWeightedHemisphere(float2 u, out float pdf)
	{
		const float phi = TWO_PI * u.y;
		const float sinTheta = sqrt(u.x);

		const float x = cos(phi) * sinTheta;
		const float y = sin(phi) * sinTheta;
		const float z = sqrt(1.0f - u.x); // = cos(theta)

		pdf = z * ONE_OVER_PI; // = cos(theta) / PI

		return float3(x, y, z);
	}

	//--------------------------------------------------------------------------------------
	// Sampling Shapes
	//--------------------------------------------------------------------------------------

	// Returns samples about the (0, 0, 1) axis
	float3 UniformSampleCone(float2 u, float cosThetaMax, out float pdf)
	{
		const float phi = TWO_PI * u.y;

		const float cosTheta = 1.0f - u.x + u.x * cosThetaMax;
		const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

		// x = sin(theta) * cos(phi)
		// y = sin(theta) * sin(phi)
		// z = cos(theta)
		const float x = cos(phi) * sinTheta;
		const float y = sin(phi) * sinTheta;
		const float z = cosTheta;

		// w.r.t. solid angle
		pdf = ONE_OVER_2_PI * rcp(1.0f - cosThetaMax);

		return float3(x, y, z);
	}

	// Area = PI;
	float2 UniformSampleDisk(float2 u)
	{
		const float r = sqrt(u.x);
		const float phi = TWO_PI * u.y;

		return float2(r * cos(phi), r * sin(phi));
	}

	// Area = PI;
	float2 UniformSampleDiskConcentricMapping(float2 u)
	{
		float a = 2.0f * u.x - 1.0f;
		float b = 2.0f * u.y - 1.0f;

		float r;
		float phi;
	
		if (a * a > b * b)
		{
			r = a;
			phi = PI_OVER_4 * (b / a);
		}
		else
		{
			r = b;
			phi = PI_OVER_2 - PI_OVER_4 * (a / b);
		}

		return float2(r * cos(phi), r * sin(phi));
	}

	// Area = FOUR_PI;
	float3 UniformSampleSphere(float2 u)
	{
		// Compute radius r (branchless).
		float u0 = 2.0f * u.x - 1.0f;
		float u1 = 2.0f * u.y - 1.0f;

		float d = 1.0f - (abs(u0) + abs(u1));
		float r = 1.0f - abs(d);

		// Compute phi in the first quadrant (branchless, except for the
		// division-by-zero test), using sign(u) to map the result to the
		// correct quadrant below.
		float phi = (r == 0) ? 0 : PI_OVER_4 * ((abs(u1) - abs(u0)) / r + 1.0f);
		float f = r * sqrt(2.0f - r * r);
		float x = f * sign(u0) * cos(phi);
		float y = f * sign(u1) * sin(phi);
		float z = sign(d) * (1.0f - r * r);

		return float3(x, y, z);
	}

	// For triangle with vertices V0, V1 and V2, any point inside it
	// can be expressed as:
	//		P = b0 * V0 + b1 * V1 + b2 * V2
	// where b0 + b1 + b2 = 1.
	//
	// Area = 0.5f * abs(cross(v1 - v0, v2 - v0))
	float2 UniformSampleTriangle(float2 u)
	{
#if 0
		float b1 = 1.0 - sqrt(u.x);
		float b2 = (1.0 - b1) * u.y;
		return float2(b1, b2);
#else
		// Ref: Eric Heitz. A Low-Distortion Map Between Triangle and Square. 2019.
		float b1, b2;
		
		if (u.y > u.x)
		{
			b1 = u.x * 0.5f;
			b2 = u.y - b1;
		}
		else
		{
			b2 = u.y * 0.5f;
			b1 = u.x - b2;
		}

		return float2(b1, b2);
#endif
	}

	//--------------------------------------------------------------------------------------
	// Blue noise sampler
	//
	// Ref: E. Heitz, L. Belcour, V. Ostromoukhov, D. Coeurjolly and J. Iehl, "A Low-Discrepancy 
	// Sampler that Distributes Monte Carlo Errors as a Blue Noise in Screen Space," in SIGGRAPH, 2019.
	//--------------------------------------------------------------------------------------

	// Sample index: frame number % 32
	// Sample dimension: e.g. (0, 1) for the diffuse indirect, (2, 3) for area light, etc.
	float BlueNoiseErrorDistribution(ByteAddressBuffer g_owenScrambledSobolSeq,
		ByteAddressBuffer g_rankingTile, ByteAddressBuffer g_scramblingTile,
		int pixel_i, int pixel_j, int sampleIndex, int sampleDimension)
	{
		// wrap arguments
		pixel_i = pixel_i & 127;
		pixel_j = pixel_j & 127;
		sampleIndex = sampleIndex & 255;
		sampleDimension = sampleDimension & 255;

		// xor index based on optimized ranking
		uint idxInBytes = (sampleDimension + (pixel_i + pixel_j * 128) * 8) * sizeof(uint);
		int rankedSampleIndex = sampleIndex ^ g_rankingTile.Load<uint>(idxInBytes);

		// fetch value in sequence
		idxInBytes = (sampleDimension + rankedSampleIndex * 256) * sizeof(uint);
		int value = g_owenScrambledSobolSeq.Load<uint>(idxInBytes);

		// If the dimension is optimized, xor sequence value based on optimized scrambling
		idxInBytes = ((sampleDimension & 7) + (pixel_i + pixel_j * 128) * 8) * sizeof(uint);
		value = value ^ g_scramblingTile.Load<uint>(idxInBytes);

		// convert to float and return
		float v = (0.5f + value) / 256.0f;

		return v;
	}

	float BlueNoiseErrorDistribution(StructuredBuffer<uint16_t> g_owenScrambledSobolSeq,
		StructuredBuffer<uint16_t> g_rankingTile, StructuredBuffer<uint16_t> g_scramblingTile,
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