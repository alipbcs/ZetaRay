/*
	Refs:
	1. Spatiotemporal Reservoir Resampling For Real-Time Ray Tracing with Dynamic Direct Lighting
	2. 

	Ideally, each pixel should seperately generate ligh source candidates to sample. Assuming 
	M=32 candidates per pixel and 1080p resolution, this requires 32 * 1920 * 1080 memory 
	lookups. Since sampling is stochastic, this leads to incoherent memory acceesses which
	could cause cache thrashing. 
	
	As a compromise between performance and correlated sampling, prior to ReSTIR, S number of 
	subsests each with size S_j can be pregenerated. Then for each pixel, a subset is chosen
	randomly and then M samples are taken from that subset which are coherent.
*/

#include "../Common/LightSourceFuncs.hlsli"
#include "../Common/Sampler.hlsli"
#include "../Common/FrameConstants.h"
#include "PreSampling.h"
#include "Common.h"

ConstantBuffer<cbFrameConstants> cbFrame : register(b0, space0);
ConstantBuffer<cbCandidateGeneration> cb : register(b1, space0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 pixel = uint2(THREAD_GROUP_SIZE_X % cb.Resolution.x, THREAD_GROUP_SIZE_X / cb.Resolution.y);
	
	// Seed RNG
	uint rngState = Sampler::InitRNG(pixel, cbFrame.FrameNum, cb.Resolution);
	
	float2 u = Sampler::GetUniform2D(rngState);

	// Sample a light source according to power	
	float pdf;
	uint lightIdx = SampleAliasTable(rngState, cbFrame.LightPowAliasTableDescHeapOffset, u, cbFrame.NumLights, pdf);
	StructuredBuffer<LightSource> g_inLightBuffer = ResourceDescriptorHeap[cbFrame.LightBufferDescHeapOffset];

	// Write light source's index into output
	RWStructuredBuffer<Candidate> g_out = ResourceDescriptorHeap[cb.OutputDescHeapOffset];
	g_out[DTid.x].Light = g_inLightBuffer[lightIdx];
	g_out[DTid.x].Prob = lightIdx;
}