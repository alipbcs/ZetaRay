#ifndef SAMPLING_TRANSFORMATIONS_H
#define SAMPLING_TRANSFORMATIONS_H

//	Refs:
//	1. Physically Based Rendering 3rd Ed.
//	2. Ray Tracing Gems 1, Chapter 16

#include "Common.hlsli"

//--------------------------------------------------------------------------------------
// Sampling Hemisphere
//--------------------------------------------------------------------------------------

float3 UniformSampleHemisphere(in float2 u, out float pdf)
{
	const float phi = TWO_PI * u.y;
	const float sinTheta = sqrt(1.0f - u.x * u.x);
	
	const float x = cos(phi) * sinTheta;
	const float y = sin(phi) * sinTheta;
	const float z = u.x;
	
	// w.r.t. solid angle
	pdf = ONE_DIV_TWO_PI;
		
	return float3(x, y, z);
}

float3 SampleCosineWeightedHemisphere(in float2 u, out float pdf)
{
	const float phi = TWO_PI * u.y;
	const float sinTheta = sqrt(u.x);

	const float x = cos(phi) * sinTheta;
	const float y = sqrt(1.0f - u.x);		// equal to cos(theta)
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
	float r = sqrt(u.x);
	float phi = TWO_PI * u.y;
	
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

#endif