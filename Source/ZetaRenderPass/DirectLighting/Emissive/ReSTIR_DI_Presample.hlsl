#include "ReSTIR_DI_Reservoir.hlsli"
#include "ReSTIR_DI_Resampling.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_DI_Presampling> g_local : register(b1);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t1);
StructuredBuffer<EmissiveTriangleSample> g_aliasTable : register(t2);
RWStructuredBuffer<LightSample> g_sampleSets : register(u0);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_DI_PRESAMPLE_GROUP_DIM_X, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
	if (DTid.x >= g_local.NumTotalSamples)
		return;

	RNG rng = RNG::Init(DTid.x, g_frame.FrameNum);

	float lightSourcePdf;
	const uint emissiveIdx = RDI_Util::SampleAliasTable(g_aliasTable, g_local.NumEmissiveTriangles, rng, lightSourcePdf);

	LightSample s;
	s.Tri = g_emissives[emissiveIdx];
	s.Pdf = lightSourcePdf;
	s.Index = emissiveIdx;
		
	g_sampleSets[DTid.x] = s;
}