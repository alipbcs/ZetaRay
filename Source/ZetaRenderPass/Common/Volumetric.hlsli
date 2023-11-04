#ifndef VOLUMETRIC_H
#define VOLUMETRIC_H

// Refs:
// 1. M. Pharr and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2010.
// 2. S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," Computer Graphics Forum, 2020.
// 3. https://github.com/Fewes/MinimalAtmosphere

#include "Math.hlsli"

//--------------------------------------------------------------------------------------
// {Absorption, scattering, extinction} coefficients
//--------------------------------------------------------------------------------------

/*
static const float3 SIGMA_S_RAYLEIGH = float3(5.802f, 13.558f, 33.1f) * 1e-6f;	// 1 / m
static const float3 SIGMA_A_RAYLEIGH = float3(0.0f, 0.0f, 0.0f);				// 1 / m
static const float3 SIGMA_T_RAYLEIGH = SIGMA_S_RAYLEIGH + SIGMA_A_RAYLEIGH;
static const float SIGMA_S_MIE = 3.996f * 1e-6f;		// Mie scattering is not wavelength-dependent
static const float SIGMA_A_MIE = 4.4f * 1e-6f;
static const float SIGMA_T_MIE = SIGMA_S_MIE + SIGMA_A_MIE;
static const float3 SIGMA_S_OZONE = float3(0.0f, 0.0f, 0.0f);
static const float3 SIGMA_A_OZONE = float3(0.65f, 1.881f, 0.085f) * 1e-6f;
static const float3 SIGMA_T_OZONE = SIGMA_S_OZONE + SIGMA_A_OZONE;
*/

namespace Volumetric
{
	//--------------------------------------------------------------------------------------
	// Phase functions
	//--------------------------------------------------------------------------------------

	// Used to describe light scattering from small molecules in 
	// earth's atmoshphere. 
	// theta is the angle between incoming and scattered light
	float RayleighPhaseFunction(float cosTheta)
	{
		return 0.0596831f * (1.0f + cosTheta * cosTheta);
	}

	// Henyey-Greenstein phase function can used model Mie scattering. Used to describe light scattering
	// from dust, smoke, fog and pollution like participating media
	float PhaseHG(float cosTheta, float g)
	{
		float g2 = g * g;
		float denom = (1 - g2) / (1 + g2 - 2 * g * cosTheta);
	
		return ONE_OVER_4_PI * g2 / (denom * sqrt(denom));
	}

	// An approximation Henyey-Greenstein phase function that is faster to compute
	float SchlickPhaseFunction(float cosTheta, float g)
	{
		float k = 1.55f * g - 0.55f * g * g * g;
		float denom = 1.0f - k * cosTheta;
	
		return ONE_OVER_4_PI * (1.0f - k * k) / (denom * denom);
	}

	//--------------------------------------------------------------------------------------
	// Density functions for heterogeneous medium
	//--------------------------------------------------------------------------------------

	// Spatial density of participating media for Rayleigh scattering. Altitude should be in km
	float DensityRayleigh(float altitude)
	{
		return exp(-max(0.0f, altitude / 8.0f));
	}

	// Spatial density of participating media for Rayleigh scattering. Altitude should be in km
	float DensityMie(float altitude)
	{
		return exp(-max(0.0f, altitude / 1.2f));
	}

	// Spatial density of ozone. Altitude should be in km
	float DensityOzone(float altitude)
	{
		return max(0, 1 - abs(altitude - 25.0f) / 15.0f);
	}

	// Altitude shouldbe in km
	float3 ComputeDensity(float altitude)
	{
		return float3(DensityRayleigh(altitude), DensityMie(altitude), DensityOzone(altitude));
	}

	//--------------------------------------------------------------------------------------
	// Estimating absorption, in-scattering and other helper functions
	//--------------------------------------------------------------------------------------

	// both arguments must be in units of km
	float ComputeAltitude(float3 pos, float planetRadius)
	{
		// center is assumed to be (0, 0, 0)
		return length(pos) - planetRadius;
	}

	// Ref: C. Ericson, Real-time Collision Detection, Morgan Kaufmann, 2005.
	float IntersectRayAtmosphere(float radius, float3 rayOrigin, float3 rayDir)
	{
		// center is assumed to be (0, 0, 0)
		float mDotdir = dot(rayDir, rayOrigin);
		float delta = mDotdir * mDotdir - dot(rayOrigin, rayOrigin) + radius * radius;
	
		// here, ray is always starting inside the sphere, so there's one negative answer and
		// one positive answer, the latter is what's intended	
		delta = sqrt(delta);
		return -mDotdir + delta;
	}

	bool IntersectRayPlanet(float radius, float3 rayOrigin, float3 rayDir, out float t)
	{
		// center is assumed to be (0, 0, 0)
		float mDotdir = dot(rayDir, rayOrigin);
		float delta = mDotdir * mDotdir - dot(rayOrigin, rayOrigin) + radius * radius;
	
		if (delta < 0.0f)
		{
			t = 0;			 
			return false;
		}
	
		// here, ray is always starting outside the sphere and the first intersection is what's intended
		delta = sqrt(delta);
		t = min(-mDotdir - delta, -mDotdir + delta);
	
		return t >= 0.0f;
	}

	// Estimates optical thickness integral by assuming extinction function sigmat_t
	// is piecewise constant. Next, Riemann sum is used to estimate the integral
	// of sigma_t along the given ray
	float3 EstimateTransmittance(float planetRadius, float3 rayOrigin, float3 rayDir, float t,
		float3 sigma_t_rayleigh, float sigma_t_mie, float3 sigma_t_ozone, int numSteps)
	{
		if (t <= 1e-5f)
			return 1.0.xxx;
	
		const float stepSize = t / numSteps;
		float3 pos = rayOrigin + 0.5f * stepSize * rayDir;
	//	float3 pos = rayOrigin;
		float3 opticalThickness = 0.0f.xxx;
	
		// Riemann sum
		[loop]
		for (int s = 0; s < numSteps; s++)
		{
			float altitude = ComputeAltitude(pos, planetRadius);
			float3 density = ComputeDensity(altitude); // altitude must be in units of km
			opticalThickness += density;
			pos += stepSize * rayDir;
		}
	
		opticalThickness = sigma_t_rayleigh * opticalThickness.x +
		sigma_t_mie * opticalThickness.y +
		sigma_t_ozone * opticalThickness.z;
	
		// midpoint rule
		opticalThickness *= stepSize;
	
		// trapezoid rule
	//	opticalThickness *= stepSize * 0.5f;

		return exp(-opticalThickness);
	}

	// Estimates in-scattered light from one directional light source such as sun
	float3 EstimateLs(float planetRadius, float3 rayOrigin, float3 rayDir, float3 lightDir, float atmosphereHeight, float g,
		float3 sigma_s_rayleigh, float sigma_s_mie, float sigma_t_mie, float3 sigma_t_ozone, int numSteps)
	{
		float t = IntersectRayAtmosphere(planetRadius + atmosphereHeight, rayOrigin, rayDir);
	
		float tPlanet;
		bool intersectedPlanet = IntersectRayPlanet(planetRadius, rayOrigin, rayDir, tPlanet);
	
		if (intersectedPlanet)
		{
			t = tPlanet;
		}
	
		const float3 atmosphereIntersection = rayOrigin + t * rayDir;
	
		const float stepSize = t / numSteps;
		float3 pos = rayOrigin + 0.5f * stepSize * rayDir;
		
		float3 opticalThickness = 0.0.xxx;
		float3 LsRayleigh = 0.0.xxx;
		float3 LsMie = 0.0.xxx;
	
		// Riemann sum
		[loop]
		for (int s = 0; s < numSteps; s++)
		{
			float altitude = ComputeAltitude(pos, planetRadius);
			float3 density = ComputeDensity(altitude);
			opticalThickness += density * stepSize;
		
			// Rayleigh scattering doesn't have absorption so sigma_t == sigma_s
			float3 rayOriginToPosTr = exp(-(sigma_s_rayleigh * opticalThickness.x +
			sigma_t_mie * opticalThickness.y +
			sigma_t_ozone * opticalThickness.z));
		
			const float posToAtmosphereDist = IntersectRayAtmosphere(planetRadius + atmosphereHeight, pos, -lightDir);
			float3 LoTranmittance = EstimateTransmittance(planetRadius, pos, -lightDir, posToAtmosphereDist,
			sigma_s_rayleigh, sigma_t_mie, sigma_t_ozone, 8);
		
			// for ray semgment from p1, p2, p3 
			// Transmittance(p1, p3) = Transmittance(p1, p2) * Transmittance(p2, p3) where p2 is in the 
			LsRayleigh += rayOriginToPosTr * density.x * LoTranmittance;
			LsMie += rayOriginToPosTr * density.y * LoTranmittance;
				
			pos += stepSize * rayDir;
		}

		// Following three are constant due to the nature of directional light sources
		const float cosTheta = dot(lightDir, -rayDir);
		const float phaseRayleigh = RayleighPhaseFunction(cosTheta);
		const float phaseMie = SchlickPhaseFunction(cosTheta, g);

		float3 Ls = LsRayleigh * sigma_s_rayleigh * phaseRayleigh;
		Ls += LsMie * sigma_s_mie * phaseMie;
		Ls *= stepSize;
	
		return Ls;
	}
}

#endif