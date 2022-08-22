#ifndef LIGHTSOURCE_FUNCS_H
#define LIGHTSOURCE_FUNCS_H

// Refs:
// 1. Physically Based Rendering 3rd Ed.
// 2. Moving Frostbite to Physically Based Rendering
// 3. Ray Tracing Gems 1, Chapter 16

#include "HLSLCompat.h"
#include "Common.hlsli"
#include "LightSourceData.h"
#include "Material.h"
#include "SamplingTransformations.hlsli"
#include "StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Converts density over area to density over solid angle from the given reference point. 
// Assuming shape S has area A, unifrom sampling w.r.t. area has pdf of 1 / A.
// Since incoming is estimated w.r.t solid angle (w), sampling w.r.t w must sum to 1:
//		p(w) = 1
//		int_w p(w) dw = 1
//		int_A p(A) (cos(theta) / r^2) * dA = 1
//		(cos(theta) / r^2) * int_A p(w) dA = 1
//		(cos(theta) / r^2) * A * p(w) = 1
//		p(w) = r^2 / (cos(theta) * A) 
//
// lightSurfaceNormal is assumed to be normalized
//--------------------------------------------------------------------------------------

float DensityOverAreaToDensityOverSolidAngle(in float3 lightSurfaceNormal,
	in float3 lightSurfacePointToReference,
	in float area,
	out float dist)
{
	float cosTheta = dot(lightSurfacePointToReference, lightSurfaceNormal);
	float rSq = dot(lightSurfacePointToReference, lightSurfacePointToReference);
	dist = sqrt(rSq);
	
	return rSq * rcp(cosTheta * area);
}

//--------------------------------------------------------------------------------------
// Samples given analytical light source
//--------------------------------------------------------------------------------------

float3 SampleLeAnalytical(in AnalyticalLightSource light, in float3 posW, in float2 u,
	out float3 wi, out float pdf, out float area, out float T, out float3 lightSurfacePos)
{
	// Given light source l_d, direct lighting is calculated as:
	//		L(wo) = int_H^2 (L_d(wi) * BRDF(wi, wo) * cos(theta) d_wi)
	//
	// With MC estimate:
	//		L(wo) = sigma(L_d(wi) * BRDF(wo, wi) * cos(theta) / pdf(wi))
	// 
	// For Area Lights, points on their surface are sampled uniformly with 
	// probability 1 / A.
	// Since PDF in MC estimate is w.r.t. solid angle, it needs to be adjusted
	// accordingly:
	//		dw / dA = cos(theta') / r^2
	// 
	// Putting everything together:
	//		int_H^2(c dw) = 1
	//		int_H^2(c * (cos(theta') / r^2) dA) = 1
	//		c * (cos(theta') / r^2) * int_H^2(dA) = 1
	//		c * (cos(theta') / r^2) * Area = 1
	//		pdf = c = r^2 / (cos(theta') * Area)
	//
	//		L(wo) = L_d(wi) * BRDF(wo, wi) * cos(theta) / (r^2 / (cos(theta') * Area))
	//
	// Furthermore, for area lights radiance can be calculated from surface power (assuming diffuse
	// distribution of radiance for each point):
	//		L_d(wi) = LuminousPower / (int_H^2(cos(theta) dw) * A
	//		L_d(wi) = LuminousPower / PI * A

	float3 Le = float3(0.0f, 0.0f, 0.0f);
	pdf = 0.0f;
	area = 0.0f;
	/*
	if (light.Type == LightSourceType::DIRECTIONAL)
	{
		Le = light.Illuminance * light.Color;
		wi = -light.Dir;
		T = FLT_MAX;
		pdf = 1.0f;
	}
	else if (light.Type == LightSourceType::POINT)
	{
		// intensity = luminous power per unit solid angle emitted by a point light source in a particular direction
		// integral over all sphere = 4 * PI
		float luminousIntensity = light.LuminousPower * ONE_DIV_FOUR_PI;

	    // inverse law square
		// light.Translation == light pos (origin is (0, 0, 0))
		float3 lightToPos = posW - light.Translation;
		float distSq = dot(lightToPos, lightToPos);
		float att = rcp(max(distSq, 0.0001f)); // 0.0001f = 1cm * 1cm

		Le = luminousIntensity * att * light.Color;
		
		// distance to the light surface
		T = sqrt(distSq);
		wi = normalize(light.Translation - posW);
		pdf = 1.0f;

		// light.Translation == light pos (origin is (0, 0, 0))
		lightSurfacePos = light.Translation;
	}
	else if (light.Type == LightSourceType::SPOT)
	{
		wi = normalize(light.Translation - posW);		
		float cosTheta = dot(-wi, light.Dir);
		
		// Light falloff
		float att = cosTheta > light.CosFalloffStart ? 1.0f : 
					(cosTheta - light.CosTotalWidth) / (light.CosFalloffStart - light.CosTotalWidth);
		att *= cosTheta > light.CosTotalWidth;
		att *= att;
		att *= att;

		// inverse law square
		float3 lightToPos = posW - light.Translation;
		float distSq = dot(lightToPos, lightToPos);
		att *= rcp(max(distSq, 0.0001f)); // 0.0001f = 1cm * 1cm
		
		// Luminous Intensity (I) is computed as:
		//		I = Total Power (lumen) / (solid angle subtended by the spotlight)
		// Solid angle is defined as
		//		 w = A / r^2 
		// where A is the spherical surface area and r is its radius
		//
		// Therefore, solid angle subtended by a cone with apex angle theta is:
		//		w = int 1 dw
		// which is the same as integrating area on the unit sphere where differential surface 
		// area is sin(theta) d_theta d_phi:
		//		w = int_0^(2 * PI) (int_0^theta sin(theta) d_theta) d_phi
		//		w = 2 * PI * (1 - cos(theta))
		// cosine of spread angle theta can be approximated as:
		//		0.5 * (CosTotalWidth + light.CosFalloffStart)
		float luminousIntensity = light.LuminousPower * rcp(TWO_PI * (1.0f - 0.5f * (light.CosTotalWidth + light.CosFalloffStart)));
		Le = luminousIntensity * att * light.Color;

		// distance to the light surface
		T = sqrt(distSq);
		pdf = 1.0f;
		
		// light.Translation == light pos (origin is (0, 0, 0))
		lightSurfacePos = light.Translation;
	}
	else if (light.Type == LightSourceType::RECTANGLE)
	{
		// uniform point on the light surface
		lightSurfacePos = float3(-0.5f * light.Dim + u * light.Dim, 0.0f); // (-w/2, -h/2) to (+w/2, +h/2)
		lightSurfacePos = mul(lightSurfacePos, light.SR);
		lightSurfacePos += light.Translation;
				
		wi = normalize(lightSurfacePos - posW);
		
		// surface normal in local coordinate system == (0, 1, 0)
		float3 lightSurfaceNormal = RotateVector(float3(0.0f, 1.0f, 0.0f), light.RotQuat);
		
		// return early if light is backfacing
		if (dot(lightSurfaceNormal, wi) <= 0)
		{
			return Le;
		}

		// intergral should be over solid angle
		float3 toLightSurface = posW - lightSurfacePos;
		area = light.Dim.x * light.Dim.y;
		pdf = DensityOverAreaToDensityOverSolidAngle(lightSurfaceNormal, toLightSurface, area, T);
		
		Le = light.Color * light.LuminousPower * ONE_DIV_PI * rcp(area);
	}
	else if (light.Type == LightSourceType::DISK)
	{
		// sample is within disk with unit area
		float2 dxdy = UniformSampleDiskConcentricMapping(u);
		lightSurfacePos = float3(dxdy * light.Radius, 0.0f);
		lightSurfacePos = mul(lightSurfacePos, light.SR);
		lightSurfacePos += light.Translation;
				
		wi = normalize(lightSurfacePos - posW);
		
		// surface normal in local coordinate system == (0, 1, 0)
		float3 lightSurfaceNormal = RotateVector(float3(0.0f, 1.0f, 0.0f), light.RotQuat);

		// return early if light is backfacing
		if (dot(lightSurfaceNormal, wi) <= 0)
		{
			return Le;
		}

		// intergral should be over solid angle
		float3 toLightSurface = posW - light.Translation;
		area = PI * light.Radius * light.Radius;
		pdf = DensityOverAreaToDensityOverSolidAngle(lightSurfaceNormal, toLightSurface, area, T);
		
		Le = light.Color * light.LuminousPower * ONE_DIV_PI * rcp(area);
	}
	*/
	return Le;
}

//--------------------------------------------------------------------------------------
// Computes intensity arriving from specified light source from the given direction.
// Visibility is caller's responsibility
//--------------------------------------------------------------------------------------

float3 ComputeLeAnalytical(in AnalyticalLightSource light, in float3 posW, in float3 lightSurfacePos,
	out float T)
{
	float3 Le = float3(0.0f, 0.0f, 0.0f);
	float3 wi = normalize(lightSurfacePos - posW);
	T = 0.0f;
	
	if (light.Type == LightSourceType::DIRECTIONAL)
	{
		Le = light.Illuminance * light.Color;
	}
	/*
	else if (light.Type == LightSourceType::POINT)
	{
		float luminousIntensity = light.LuminousPower * ONE_DIV_FOUR_PI;
		float3 lightToPos = posW - light.Translation;
		float distSq = dot(lightToPos, lightToPos);
		float att = rcp(max(distSq, 0.0001f)); // 0.0001f = 1cm * 1cm

		Le = luminousIntensity * att * light.Color;
		T = sqrt(distSq);
	}
	else if (light.Type == LightSourceType::SPOT)
	{
		wi = normalize(light.Translation - posW);
		float cosTheta = dot(-wi, light.Dir);

		float att = cosTheta > light.CosFalloffStart ? 1.0f : 
					(cosTheta - light.CosTotalWidth) / (light.CosFalloffStart - light.CosTotalWidth);
		att *= cosTheta > light.CosTotalWidth;
		att *= att;
		att *= att;

		float3 lightToPos = posW - light.Translation;
		float distSq = dot(lightToPos, lightToPos);
		att *= rcp(max(distSq, 0.0001f));
		
		float luminousIntensity = light.LuminousPower * rcp(TWO_PI * (1.0f - 0.5f * (light.CosTotalWidth + light.CosFalloffStart)));
		Le = luminousIntensity * att * light.Color;		
		T = sqrt(distSq);
	}
	else if (light.Type == LightSourceType::RECTANGLE)
	{
		float3 lightSurfaceNormal = RotateVector(float3(0.0f, 1.0f, 0.0f), light.RotQuat);

		if (dot(lightSurfaceNormal, wi) <= 0)
		{
			return Le;
		}
		
		float area = light.Dim.x * light.Dim.y;
		Le = light.Color * light.LuminousPower * ONE_DIV_PI * rcp(area);
		T = distance(lightSurfacePos, posW);
	}
	else if (light.Type == LightSourceType::DISK)
	{
		float3 lightSurfaceNormal = RotateVector(float3(0.0f, 1.0f, 1.0f), light.RotQuat);

		if (dot(lightSurfaceNormal, wi) <= 0)
		{
			return Le;
		}
		
		float area = PI * light.Radius * light.Radius;
		Le = light.Color * light.LuminousPower * ONE_DIV_PI * rcp(area);
		T = distance(lightSurfacePos, posW);
	}
	*/
	return Le;
}

//--------------------------------------------------------------------------------------
// Samples given emissive triangle
//--------------------------------------------------------------------------------------

float3 SampleLeEmissiveTri(in EmissiveTriangle light, in float3 posW, in float2 u,
	in uint emissiveMapsHeapOffset, out float3 wi, out float pdf, out float area, out float T,
	out float3 lightSurfacePos, out half2 barry)
{
	float3 Le = float3(0.0f, 0.0f, 0.0f);
	pdf = 0.0f;
	area = 0.0f;

	StructuredBuffer<Vertex> vertexBuffer = ResourceDescriptorHeap[light.DescHeapIdx];
	StructuredBuffer<INDEX_TYPE> indexBuffer = ResourceDescriptorHeap[light.DescHeapIdx + 1];

	INDEX_TYPE offset = INDEX_TYPE(light.PrimitiveIdx * 3);
	
	INDEX_TYPE i0 = indexBuffer[offset];
	INDEX_TYPE i1 = indexBuffer[offset + 1];
	INDEX_TYPE i2 = indexBuffer[offset + 2];
	
	Vertex v0 = vertexBuffer[i0];
	Vertex v1 = vertexBuffer[i1];
	Vertex v2 = vertexBuffer[i2];

	float3 v0W = mul(v0.PosL, light.SR);
	v0W += light.Translation;
		
	float3 v1W = mul(v1.PosL, light.SR);
	v1W += light.Translation;
		
	float3 v2W = mul(v2.PosL, light.SR);
	v2W += light.Translation;
		
	// sample the light surface in world space
	float3 barryCoords = UniformSampleTriangle(u);
	float3 sampledLightSurfacePos = barryCoords.x * v0W + barryCoords.y * v1W + barryCoords.z * v2W;
		
	// store barrycentric coords, position can be reconstructed from it
//		lightSurfacePos = barry;
	lightSurfacePos = sampledLightSurfacePos;
	barry = half2(barryCoords.x, barryCoords.y);
	wi = normalize(sampledLightSurfacePos - posW);

	// distance to the light surface pos
	T = distance(lightSurfacePos, posW);
		
	// == geometric normal
	float3 edge0CrossEdge1 = cross(v1W - v0W, v2W - v0W);
	float3 lightSurfaceNormal = normalize(edge0CrossEdge1);

	// return early if light is backfacing
	if (dot(lightSurfaceNormal, wi) <= 0)
	{
		return Le;
	}
		
	area = 0.5f * length(edge0CrossEdge1);		
	pdf = DensityOverAreaToDensityOverSolidAngle(lightSurfaceNormal, posW - sampledLightSurfacePos, area, T);

	// Emissive light
	EMISSIVE_MAP g_inEmissiveMap = ResourceDescriptorHeap[emissiveMapsHeapOffset + light.EmissiveMapIdx];
		
	float2 uv = barryCoords.x * v0.TexUV + barryCoords.y * v1.TexUV + barryCoords.z * v2.TexUV;
	float3 l = g_inEmissiveMap.SampleLevel(g_samLinearClamp, uv, 0.0f).xyz;
		
	Le = l * ONE_DIV_PI * rcp(area);

	return Le;
}

//--------------------------------------------------------------------------------------
// Returns light received from an emissive triangle
//--------------------------------------------------------------------------------------

half3 ComputeLeEmissiveTri(in EmissiveTriangle light, in float3 posW, in uint emissiveMapsHeapOffset,
	in float3 lightSurfacePos, in half2 barryCoords)
{
	half3 Le = half3(0.0h, 0.0h, 0.0h);
	float3 wi = normalize(lightSurfacePos - posW);

	StructuredBuffer<Vertex> vertexBuffer = ResourceDescriptorHeap[light.DescHeapIdx];
	StructuredBuffer<INDEX_TYPE> indexBuffer = ResourceDescriptorHeap[light.DescHeapIdx + 1];

	INDEX_TYPE offset = INDEX_TYPE(light.PrimitiveIdx * 3);
	
	INDEX_TYPE i0 = indexBuffer[offset];
	INDEX_TYPE i1 = indexBuffer[offset + 1];
	INDEX_TYPE i2 = indexBuffer[offset + 2];
	
	Vertex v0 = vertexBuffer[i0];
	Vertex v1 = vertexBuffer[i1];
	Vertex v2 = vertexBuffer[i2];

	float3 v0W = mul(v0.PosL, light.SR);
	v0W += light.Translation;
		
	float3 v1W = mul(v1.PosL, light.SR);
	v1W += light.Translation;
		
	float3 v2W = mul(v2.PosL, light.SR);
	v2W += light.Translation;
		
	// == geometric normal
	float3 edge0CrossEdge1 = cross(v1W - v0W, v2W - v0W);
	float3 lightSurfaceNormal = normalize(edge0CrossEdge1);

	// return early if light is backfacing
	if (dot(lightSurfaceNormal, wi) <= 0)
	{
		return Le;
	}

	EMISSIVE_MAP g_inEmissiveMap = ResourceDescriptorHeap[emissiveMapsHeapOffset + light.EmissiveMapIdx];
		
	// emissive textures contain luminance in units of cd / m^2
	half3 barry = half3(barryCoords.x, barryCoords.y, 1.0h - barryCoords.x - barryCoords.y);
	float2 uv = barry.x * v0.TexUV + barry.y * v1.TexUV + barry.z * v2.TexUV;
	Le = g_inEmissiveMap.SampleLevel(g_samLinearClamp, uv, 0.0f).rgb;
		
	return Le;
}

// Samples scene lights sources based on the distribution of respective powers (measured in units of lumen).
// Sampling is done using an Alias Table. With this approach, light power pdf
// can be sampled with one memory access.
// Ref: https://www.keithschwarz.com/darts-dice-coins/
uint SampleAliasTable(in uint aliasTabledescHeapOffset, in float2 u, in uint numLights, out float pdf)
{
	uint ret;
	
	StructuredBuffer<AliasTableEntry> g_inLightPowAliasTable = ResourceDescriptorHeap[aliasTabledescHeapOffset];
	
	uint idx = uint(u.x * numLights);
	AliasTableEntry e = g_inLightPowAliasTable[idx];
	
	// bugged
//	bool pickAlias = u.y < e.P;
//	ret = pickAlias ? e.Alias : idx;
//	pdf = pickAlias ? g_inLightPowAliasTable[e.Alias].OriginalP : e.OriginalP;
	
	return ret;
}

uint SampleEnvMapAliasTable(StructuredBuffer<AliasTableEntry> g_envMapAliasTable, in float2 u, in uint numPatches, out float pdf)
{
	uint ret;
	
	uint idx = uint(u.x * numPatches);
	AliasTableEntry e = g_envMapAliasTable[idx];
	
	bool pickCurr = u.y < e.P;
	ret = pickCurr ? idx : e.Alias;
	pdf = pickCurr ? e.OriginalProb : g_envMapAliasTable[e.Alias].OriginalProb;
	
	return ret;
}

float2 SampleEnvMapPatch(in EnvMapPatch patch, in float2 u, in uint texW, in uint texHDiv2, in float dPhi)
{
//	float phi = patch.Phi1 + u.x * (patch.Phi2 - patch.Phi1);
	float phi = lerp(patch.Phi1, patch.Phi1 + dPhi, u.x);

	float cosTheta = patch.CosTheta1 - u.y * (patch.CosTheta1 - patch.CosTheta2);
	float theta = ArcCos(cosTheta);

	float2 uv;
	uv.x = phi * ONE_DIV_TWO_PI * texW;
	uv.y = theta * TWO_DIV_PI * texHDiv2;

	return uv;
}

#endif