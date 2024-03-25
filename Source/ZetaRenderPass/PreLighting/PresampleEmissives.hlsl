#include "PreLighting_Common.h"
#include "../Common/FrameConstants.h"
#include "../Common/LightSource.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbPresampling> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t0);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t1);
RWStructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(u0);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(PRESAMPLE_EMISSIVE_GROUP_DIM_X, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
    if (DTid.x >= g_local.NumTotalSamples)
        return;

    RNG rng = RNG::Init(DTid.x, g_frame.FrameNum);

    Light::AliasTableSample entry = Light::AliasTableSample::get(g_aliasTable, 
        g_frame.NumEmissiveTriangles, rng);
    RT::EmissiveTriangle tri = g_emissives[entry.idx];
    Light::EmissiveTriSample lightSample = Light::EmissiveTriSample::get(/*unused*/ 0, tri, rng, false);

    float3 le = Light::Le_EmissiveTriangle(tri, lightSample.bary, g_frame.EmissiveMapsDescHeapOffset);

    RT::PresampledEmissiveTriangle s;
    s.pos = lightSample.pos;
    s.normal = Math::EncodeOct32(lightSample.normal);
    s.le = half3(le);
    s.bary = Math::EncodeAsUNorm2(lightSample.bary);;
    s.twoSided = tri.IsDoubleSided();
    s.idx = entry.idx;
    s.ID = tri.ID;
    s.pdf = entry.pdf * lightSample.pdf;

    g_sampleSets[DTid.x] = s;
}