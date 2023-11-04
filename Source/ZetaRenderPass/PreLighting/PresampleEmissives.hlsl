#include "PreLighting_Common.h"
#include "../Common/FrameConstants.h"
#include "../Common/RT.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbPresampling> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t0);
StructuredBuffer<RT::EmissiveTriangleSample> g_aliasTable : register(t1);
RWStructuredBuffer<RT::LightSample> g_sampleSets : register(u0);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(PRESAMPLE_EMISSIVE_GROUP_DIM_X, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
	if (DTid.x >= g_local.NumTotalSamples)
		return;

	RNG rng = RNG::Init(DTid.x, g_frame.FrameNum);

	float lightSourcePdf;
	const uint emissiveIdx = RT::SampleAliasTable(g_aliasTable, g_local.NumEmissiveTriangles, rng, lightSourcePdf);

	RT::LightSample s;
	s.Tri = g_emissives[emissiveIdx];
	s.Pdf = lightSourcePdf;
	s.Index = emissiveIdx;

	g_sampleSets[DTid.x] = s;
}