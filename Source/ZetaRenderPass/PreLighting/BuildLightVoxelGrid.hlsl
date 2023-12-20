#include "PreLighting_Common.h"
#include "../Common/RT.hlsli"
#include "../Common/LightVoxelGrid.hlsli"

#define NUM_CANDIDATES 6

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbLVG> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t0);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t1);
RWStructuredBuffer<RT::VoxelSample> g_voxel : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float3 AdjustLightPos(float3 pos, float3 c, float3 extents, out bool inside)
{
    float3 d = abs(pos - c);
    inside = d.x <= extents.x && d.y <= extents.y && d.z <= extents.z;
    
    if(!inside)
        return pos;

    int maxIdx = d.x >= d.y ? (d.x >= d.z ? 0 : 2) : (d.y >= d.z ? 1 : 2);
    float3 posSnapped = pos;
    posSnapped[maxIdx] = extents[maxIdx];

    return posSnapped;
}

bool IsBackfacing(float3 lightPos, float3 lightNormal, float3 corners[8])
{
    [unroll]
    for(int i = 0; i < 8; i++)
    {
        if(dot(corners[i] - lightPos, lightNormal) <= 0)
            return true;
    }

    return false;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

static const int MinWaveSize = 16;
static const int MaxNumWaves = NUM_SAMPLES_PER_VOXEL / MinWaveSize;
groupshared float g_waveSum[MaxNumWaves];
groupshared uint16_t g_waveNumLights[MaxNumWaves];

[numthreads(NUM_SAMPLES_PER_VOXEL, 1, 1)]
void main(uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
    const uint3 gridDim = uint3(g_local.GridDim_x, g_local.GridDim_y, g_local.GridDim_z);   
    const uint gridStart = LVG::FlattenVoxelIndex(Gid, gridDim);

    // if (gridStart * NUM_SAMPLES_PER_VOXEL + Gidx >= g_local.NumTotalSamples)
    //     return;

    const float3 extents = float3(g_local.Extents_x, g_local.Extents_y, g_local.Extents_z);
    RNG rng = RNG::Init(gridStart * NUM_SAMPLES_PER_VOXEL + Gidx, g_frame.FrameNum);
    const float3 voxelCenter = LVG::VoxelCenter(Gid, gridDim, extents, g_frame.CurrViewInv, g_local.Offset_y);

    RT::VoxelSample r;
    r.pos = FLT_MAX;
    r.normal = 0;
    r.le = 0;
    r.pdf = 0;
    r.twoSided = false;
    r.ID = uint32_t(-1);

    float w_sum = 0;
    float target_z = 0;

    float3 corners[8];
    corners[0] = float3(-1, -1, -1);
    corners[1] = float3(-1, -1, 1);
    corners[2] = float3(-1, 1, -1);
    corners[3] = float3(-1, 1, 1);
    corners[4] = float3(1, -1, -1);
    corners[5] = float3(1, -1, 1);
    corners[6] = float3(1, 1, -1);
    corners[7] = float3(1, 1, 1);
    
    [unroll]
    for(int i = 0; i < 8; i++)
        corners[i] = voxelCenter + corners[i] * extents;

    uint16_t numLights = 0;

    for(int i = 0; i < NUM_CANDIDATES; i++)
    {
        float lightSourcePdf;
        const uint emissiveIdx = Light::SampleAliasTable(g_aliasTable, g_frame.NumEmissiveTriangles, 
           rng, lightSourcePdf);

        RT::EmissiveTriangle emissive = g_emissives[emissiveIdx];
        const Light::EmissiveTriAreaSample lightSample = Light::SampleEmissiveTriangleSurface(voxelCenter, 
            emissive, rng, false);

        const float3 le = Light::Le_EmissiveTriangle(emissive, lightSample.bary, g_frame.EmissiveMapsDescHeapOffset);
        
        // "Snap" lights inside the voxel to its boundary planes.
        bool inside;
        const float3 lightPos = AdjustLightPos(lightSample.pos, voxelCenter, extents, inside);

        // Cull backfacing lights, but only if they're not inside the voxel.
        if(!inside && !emissive.IsDoubleSided())
        {
            if(IsBackfacing(lightSample.pos, lightSample.normal, corners))
                continue;
        }

        const float t = length(lightPos - voxelCenter);

        const float target = Math::Luminance(le) / max(t * t, 1e-6);
        const float lightPdf = lightSourcePdf * lightSample.pdf;
        float w = target / max(lightPdf, 1e-6);
        w_sum += w;

        if(rng.Uniform() < w / max(w_sum, 1e-6))
        {
            r.pos = lightSample.pos;
            r.normal = Math::EncodeOct16(lightSample.normal);
            r.le = half3(le);
            r.twoSided = emissive.IsDoubleSided();
            r.ID = emissive.ID;
            target_z = target;
        }

        numLights++;
    }

    const int numLanesInWave = WaveGetLaneCount();
    const int wave = Gidx / numLanesInWave;
    const int numWavesInGroup = NUM_SAMPLES_PER_VOXEL / numLanesInWave;
    float w_sum_group = WaveActiveSum(w_sum);
    uint16_t numLightsGroup = WaveActiveSum(numLights);
    
    if (WaveIsFirstLane())
    {
        g_waveSum[wave] = w_sum_group;
        g_waveNumLights[wave] = numLightsGroup;
    }
    
    GroupMemoryBarrierWithGroupSync();

    const uint GidxModWaveLen = Gidx - numLanesInWave * wave;
    w_sum_group = GidxModWaveLen < numWavesInGroup ? g_waveSum[GidxModWaveLen] : 0.0;
    numLightsGroup = GidxModWaveLen < numWavesInGroup ? g_waveNumLights[GidxModWaveLen] : 0;
    // Assuming min wave size of 16, there are at most NUM_SAMPLES_PER_VOXEL / 16 values to 
    // add together, so one WaveActiveSum would be enough as long as NUM_SAMPLES_PER_VOXEL <= 256.
    w_sum_group = WaveActiveSum(w_sum_group);
    numLightsGroup = WaveActiveSum(numLightsGroup);

    w_sum_group /= numLightsGroup;
    r.pdf = target_z / max(w_sum_group, 1e-6);

    g_voxel[gridStart * NUM_SAMPLES_PER_VOXEL + Gidx] = r;
}