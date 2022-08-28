#include "../Common/Common.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/VolumetricLighting.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/Sampler.hlsli"
#include "Sky_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbSky> g_local : register(b0, space0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1, space0);
RaytracingAccelerationStructure g_sceneBVH : register(t0, space0);

//--------------------------------------------------------------------------------------
// Globals
//--------------------------------------------------------------------------------------

// assumes wave size of at least 32
#define WAVE_SIZE 32 
#define LOG_WAVE_SIZE 5

groupshared float3 g_waveTr[WAVE_SIZE];
groupshared float3 g_waveLs[WAVE_SIZE];

static const float Halton[8] = { 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f };

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float GetVoxelLinearDepth(uint voxelZ)
{
//	float z = g_local.VoxelGridNearZ + ((float) voxelZ / NUM_SLICES) * (g_local.VoxelGridDepth - 0.03f);
//	z *= exp(-(NUM_SLICES - voxelZ - 1) / NUM_SLICES);

	float z = g_local.VoxelGridNearZ + pow((float) voxelZ / float(INSCATTERING_THREAD_GROUP_SIZE_X), g_local.DepthMappingExp) *
		(g_local.VoxelGridFarZ - g_local.VoxelGridNearZ);
	
	return z;
}

float EvaluateVisibility(float3 pos, float3 wi)
{
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
			 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			 RAY_FLAG_CULL_NON_OPAQUE> rayQuery;
		
	RayDesc ray;
	ray.Origin = pos;
	ray.TMin = 0.0f;
	ray.TMax = FLT_MAX;
	ray.Direction = wi;
	
	// initialize
	rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
	
	// traversal
	rayQuery.Proceed();

	// light source is occluded
	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		return 0.0f;
	
	return 1.0f;
}

void ComputeVoxelData(in float3 pos, in float3 sigma_t_rayleigh, in float sigma_t_mie, 
	in float3 sigma_t_ozone, out float3 LoTranmittance, out float3 density)
{
	pos.y += g_frame.PlanetRadius;
	const float altitude = ComputeAltitude(pos, g_frame.PlanetRadius);
	density = ComputeDensity(altitude);
		
	const float posToAtmosphereDist = IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, pos, -g_frame.SunDir);
	LoTranmittance = EstimateTransmittance(g_frame.PlanetRadius, pos, -g_frame.SunDir, posToAtmosphereDist,
		sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);
		
	pos.y -= g_frame.PlanetRadius;
	const float isSunVisibleFromPos = EvaluateVisibility(pos, -g_frame.SunDir);
	LoTranmittance *= isSunVisibleFromPos;
}

void Integrate(in float3 rayDir, in float ds, in float3 sigma_s_rayleigh, in float sigma_s_mie, in float sigma_t_mie,
	in float3 sigma_t_ozone, in float3 LoTranmittance, in float3 density, inout float3 waveStartToPosTr, out float3 Ls)
{
	const float3 sliceDensity = density * ds;
	const float3 opticalThickness = WavePrefixSum(sliceDensity) + sliceDensity;
	
	// transmittance from the beginning of this wave to position in this lane
	waveStartToPosTr = exp(-(sigma_s_rayleigh * opticalThickness.x +
			sigma_t_mie * opticalThickness.y +
			sigma_t_ozone * opticalThickness.z));
	
	const float3 common = waveStartToPosTr * LoTranmittance * ds;	
	const float3 sliceLsRayleigh = common * density.x;
	const float3 sliceLsMie = common * density.y;
	
	const float3 LsRayleigh = WavePrefixSum(sliceLsRayleigh) + sliceLsRayleigh;
	const float3 LsMie = WavePrefixSum(sliceLsMie) + sliceLsMie;

	// following are constant due to the nature of directional light sources
	const float cosTheta = dot(g_frame.SunDir, -rayDir);
	const float phaseRayleigh = RayleighPhaseFunction(cosTheta);
	const float phaseMie = SchlickPhaseFunction(cosTheta, g_frame.g);
	
	Ls = LsRayleigh * sigma_s_rayleigh * phaseRayleigh + LsMie * sigma_s_mie * phaseMie;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[WaveSize(WAVE_SIZE)]
[numthreads(INSCATTERING_THREAD_GROUP_SIZE_X, INSCATTERING_THREAD_GROUP_SIZE_Y, INSCATTERING_THREAD_GROUP_SIZE_Z)]
void main(uint Gidx : SV_GroupIndex, uint3 Gid : SV_GroupID)
{
	const uint3 voxelID = uint3(Gid.xy, Gidx);
	const float2 posTS = (Gid.xy + 0.5f) / float2(g_local.NumVoxelsX, g_local.NumVoxelsY);
	
	const float3 viewBasisX = g_frame.CurrView[0].xyz;
	const float3 viewBasisY = g_frame.CurrView[1].xyz;
	const float3 viewBasisZ = g_frame.CurrView[2].xyz;
	
	float2 posNDC = posTS * 2.0 - 1.0f;
	posNDC.y = -posNDC.y;
	posNDC.x *= g_frame.AspectRatio;
	posNDC *= g_frame.TanHalfFOV;
	const float3 dirV = float3(posNDC, 1.0f);
	const float3 dirW = dirV.x * viewBasisX + dirV.y * viewBasisY + dirV.z * viewBasisZ;
	
	const float3 rayDirVS = normalize(dirV);
	const float3 rayDirWS = normalize(dirW);
	
	// exponentially distrubted slices along the frustum-aligned voxel grid depth
	// 
	// |--------|------------|------------------|-------------------------|
	// |  Voxel |   Voxel    |      Voxel       |          Voxel          | ...
	//	
	// ----> z
	//
	
	// slice-depth for this voxel
	const float currSliceStartLinearDepth = GetVoxelLinearDepth(voxelID.z);
	const float nextSliceStarLineartDepth = GetVoxelLinearDepth(voxelID.z + 1);	
	const float ds = ((nextSliceStarLineartDepth - currSliceStartLinearDepth) / rayDirVS.z);
	
	// sample position
	const float sliceStartT = currSliceStartLinearDepth / rayDirVS.z;
	const float offset = Halton[g_frame.FrameNum & 7];
	float3 voxelPos = g_frame.CameraPos + rayDirWS * (sliceStartT + offset * ds);
	
	const float3 sigma_s_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

	//
	// compute voxel data
	//
	
	float3 density;
	float3 LoTransmittance;
	ComputeVoxelData(voxelPos, sigma_s_rayleigh, sigma_t_mie, sigma_t_ozone, LoTransmittance, density);
	
	//
	// integrate across wave
	//
	
	float3 tr;
	float3 Ls;
	Integrate(rayDirWS, ds, sigma_s_rayleigh, g_frame.MieSigmaS, sigma_t_mie, sigma_t_ozone, LoTransmittance, density, 
		tr, Ls);
	
	// wave size is always a power of 2
	const uint waveIdx = voxelID.z >> LOG_WAVE_SIZE;
	
	if ((Gidx & (WAVE_SIZE - 1)) == WAVE_SIZE - 1)
	{
		g_waveTr[waveIdx] = WaveReadLaneAt(tr, WaveGetLaneCount() - 1);
		g_waveLs[waveIdx] = WaveReadLaneAt(Ls, WaveGetLaneCount() - 1);
	}
	
	GroupMemoryBarrierWithGroupSync();
	
	float3 totalTr = 1.0f.xxx;
	float3 prevLs = 0.0f.xxx;
	
	// account for inscattering and transmittance from eariler waves.
	// transmittance from wave start to each voxel is already accounted for
	for (uint wave = 0; wave < waveIdx; wave++)
	{
		prevLs += g_waveLs[wave] * totalTr;		
		totalTr *= g_waveTr[wave];		// transmittance is multiplicative along a given ray
	}
	
	Ls = Ls * totalTr + prevLs;
	
	RWTexture3D<half4> g_voxelGrid = ResourceDescriptorHeap[g_local.VoxelGridDescHeapIdx];	

	// R11G11B10 doesn't have a sign bit
	Ls = max(Ls, 0.0f.xxx);
	g_voxelGrid[voxelID].xyz = half3(Ls * g_frame.SunIlluminance);
}
