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

    float lightSourcePdf;
    const uint emissiveIdx = Light::SampleAliasTable(g_aliasTable, g_frame.NumEmissiveTriangles, rng, lightSourcePdf);

    RT::EmissiveTriangle emissive = g_emissives[emissiveIdx];
    const Light::EmissiveTriAreaSample lightSample = Light::SampleEmissiveTriangleSurface(0, emissive, rng, false);

    float3 le = Light::Le_EmissiveTriangle(emissive, lightSample.bary, g_frame.EmissiveMapsDescHeapOffset);

    RT::PresampledEmissiveTriangle s;
    s.pos = lightSample.pos;
    s.normal = Math::EncodeOct16(lightSample.normal);
    s.le = half3(le);
    s.bary = Math::EncodeAsUNorm2(lightSample.bary);;
    s.twoSided = emissive.IsDoubleSided();
    s.idx = emissiveIdx;
    s.ID = emissive.ID;
    s.pdf = lightSourcePdf * lightSample.pdf;

    g_sampleSets[DTid.x] = s;
}