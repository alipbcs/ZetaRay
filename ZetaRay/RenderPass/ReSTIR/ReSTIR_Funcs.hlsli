#ifndef RESTIR_FUNCS_H
#define RESTIR_FUNCS_H

#include "ReSTIR_Common.h"
#include "../Common/BRDF.hlsli"
#include "../Common/Sampler.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

Reservoir ResetReservoir()
{
	Reservoir r;

	r.M = 0;
	r.WeightSum = 0.0f;
	r.EnvMapUV = float2(0.0f, 0.0f);
	r.TargetFunction = 0.0f;
	r.W = 0.0f;
	r.WasTemporalReservoirVisible = false;
	r.DidTemporalReuse = false;

	return r;
}

float4 ComputeTargetFunction(in Texture2D<half3> g_envMap, in float2 uv, in float cosTheta, in float phi, 
	in float3 posW, in float worldRadius, inout SurfaceInteraction surface)
{
	// sample the HDRi (BC6H_UF16)
	const half3 L_e = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f);
		
	// fill out the rest of surface now that wi is known
	const float3 lightSurfaceSample = SphericalToCartesian(worldRadius, cosTheta, phi);
	const float3 wi = normalize(lightSurfaceSample - posW);
	CompleteSurfaceInteraction(wi, surface);
	const float3 BRDFxLe = L_e * ComputeSurfaceBRDF(surface); // ndotwi is already included in ComputeSurfaceBRDF()
	float targetFunction = LuminanceFromLinearRGB(BRDFxLe);
	
	return float4(targetFunction, wi);
}

// Ref: https://digitalcommons.dartmouth.edu/dissertations/77/
void DoPairwiseMIS(in Texture2D<half3> g_envMap, in float3 posW, in float3 prevPosW, in float worldRadius, 
	in Reservoir neighbor, inout Reservoir canonical, in SurfaceInteraction neighborSurface, 
	inout SurfaceInteraction canonicalSurface, inout float m_c, inout float M, inout uint rngState)
{
	// target function for current pixel evaluated at neighbor's_sample
	float2 uv = neighbor.EnvMapUV;
	float2 phiTheta = uv * float2(TWO_PI, PI);
	float4 currTargetFuncAtNeighborY = ComputeTargetFunction(g_envMap, uv, cos(phiTheta.y), phiTheta.x, posW, worldRadius, canonicalSurface);
	
	// target function for neighbor pixel evaluated at canonical's_sample
	uv = canonical.EnvMapUV;
	phiTheta = uv * float2(TWO_PI, PI);
	float4 neighborTargetFuncAtCurrY = ComputeTargetFunction(g_envMap, uv, cos(phiTheta.y), phiTheta.x, prevPosW, worldRadius, neighborSurface);
	
	// pairwise MIS
	float p_i_neighbor_y = rcp(neighbor.W);
	float p_c_neighbor_y = currTargetFuncAtNeighborY.x * canonical.M / canonical.WeightSum;
	float p_i_curr_y = neighborTargetFuncAtCurrY.x * neighbor.M / neighbor.WeightSum;
	float p_c_curr_y = rcp(canonical.W);
	float m_i = neighbor.M * p_i_neighbor_y / (neighbor.M * p_i_neighbor_y + canonical.M * 0.5f * p_c_neighbor_y);
	m_c += 1.0f - ((neighbor.M * p_i_curr_y) / (neighbor.M * p_i_curr_y + canonical.M * 0.5f * p_c_curr_y));
	
	const float risWeight = currTargetFuncAtNeighborY.x * neighbor.W * m_i;
	canonical.WeightSum += risWeight;
	M += neighbor.M;
	const float u = RandUniform(rngState);
	const bool updateReservoir = u < (risWeight / canonical.WeightSum);
	
	if (updateReservoir)
	{
		canonical.EnvMapUV = neighbor.EnvMapUV;
		canonical.TargetFunction = currTargetFuncAtNeighborY.x;
	}
}

void EndPairwiseMIS(in float M, in float2 origUV, in float origTarget, in float origW, in uint numNeighbors,
	inout Reservoir canonical, inout float m_c, inout uint rngState)
{
	const float risWeight = origTarget * origW * m_c;
	canonical.WeightSum += risWeight;
	const float u = RandUniform(rngState);
	bool updateReservoir = u < (risWeight / canonical.WeightSum);
	
	if (updateReservoir)
	{
		canonical.EnvMapUV = origUV;
		canonical.TargetFunction = origTarget;
	}
	
	canonical.M += M;
	canonical.W = canonical.WeightSum / (canonical.TargetFunction * (1 + numNeighbors));
}

bool NormalBiasHeuristic(float3 neighborNormal, float3 currNormal, float normalAngleThreshold)
{
	float ret = dot(neighborNormal, currNormal);
	return ret <= normalAngleThreshold;
}

float4 NormalBiasHeuristic(float3 neighborNormals[4], float3 currNormal, float normalAngleThreshold)
{
	// compute angle between current normal and neighbor normlas
	float4 ret = float4(dot(neighborNormals[0], currNormal),
						dot(neighborNormals[1], currNormal),
						dot(neighborNormals[2], currNormal),
						dot(neighborNormals[3], currNormal));
	
	// reject neighbor if the angle between its normal and current pixel's normal 
	// exceeds given threshold
	ret *= (ret <= normalAngleThreshold);
	
	return ret;
}

bool DepthBiasHeuristic(float neighborDepth, float currDepth, float2 dxdy, float depthToleranceScale)
{
	// assuming a first-order approximation, estimate a depth tolerance
	// i.e. z' - currZ = dzdx(currX, currY) + dzdy(currX, currY)
	float depthTolerance = abs(dxdy.x) + abs(dxdy.y);
	depthTolerance *= depthToleranceScale;
	
	// compute relative difference between current sample and history samples
	return NextFloat32Up(currDepth + depthTolerance) >= NextFloat32Up(abs(neighborDepth - currDepth));
}

float4 DepthBiasHeuristic(float4 neighborDepths, float currDepth, float2 dxdy, float depthToleranceScale)
{
	// assuming a first-order approximation, estimate a depth tolerance
	// i.e. z' - currZ = dzdx(currX, currY) + dzdy(currX, currY)
	float depthTolerance = abs(dxdy.x) + abs(dxdy.y);
	depthTolerance *= depthToleranceScale;
	
	// compute relative difference between current sample and history samples
	float4 ret = min(NextFloat32Up(currDepth + depthTolerance) / NextFloat32Up(abs(neighborDepths - currDepth)), 1.0f);
	
	return ret;
}

#endif