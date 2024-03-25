/**********************************************************************
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/

#include "ffx_denoiser_util.hlsli"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/GBuffers.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "SunShadow_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbFFX_DNSR_Temporal> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

#define KERNEL_RADIUS 8
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.0025f

groupshared int g_FFX_DNSR_Shadows_false_count;
groupshared float g_FFX_DNSR_Shadows_neighborhood[8][24];

static const uint2 GroupDim = uint2(DNSR_TEMPORAL_THREAD_GROUP_SIZE_X, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y);

static const float KernelMulInvWeightSum[9] =
{
    0.11082928804490101,
    0.10679958437318364,
    0.0955684671165583,
    0.07941265501277292,
    0.06127662686745019,
    0.043906621756875146,
    0.02921428314803944,
    0.01805055183890165,
    0.010356565863768184
};

uint FFX_DNSR_Shadows_ReadRaytracedShadowMask(uint2 tileIdx)
{
    Texture2D<uint> g_sunShadowMask = ResourceDescriptorHeap[g_local.ShadowMaskSRVDescHeapIdx];
    uint groupMask = g_sunShadowMask[tileIdx];

    return groupMask;
}

float FFX_DNSR_Shadows_ReadDepth(uint2 DTid)
{
    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float depth = g_depth[DTid];

    return depth;
}

float FFX_DNSR_Shadows_ReadPreviousDepth(uint2 DTid)
{
    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float depth = g_prevDepth[DTid];

    return depth;
}

float3 FFX_DNSR_Shadows_ReadNormals(uint2 DTid)
{
    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[DTid]);

    return normal;
}

float2 FFX_DNSR_Shadows_ReadVelocity(uint2 DTid)
{
    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 motionVec = g_motionVector[DTid];

    return motionVec;
}

float3 FFX_DNSR_Shadows_ReadPreviousMomentsBuffer(int2 history_pos)
{
    RWTexture2D<float3> g_prevMoments = ResourceDescriptorHeap[g_local.MomentsUAVHeapIdx];
    
    if (any(history_pos < 0) || any(history_pos >= int2(g_frame.RenderWidth, g_frame.RenderHeight)))
        return 0.0.xxx;
        
    return g_prevMoments[history_pos];
}

float FFX_DNSR_Shadows_ReadHistory(float2 histUV)
{
    Texture2D<float2> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalDescHeapIdx];
    return g_prevTemporalCache.SampleLevel(g_samLinearClamp, histUV, 0).x;
}

void FFX_DNSR_Shadows_WriteTileMetadata(uint2 Gid, uint2 GTid, bool is_cleared, bool all_in_light)
{
    if (all(GTid == 0))
    {
        uint light_mask = all_in_light ? TILE_META_DATA_LIGHT_MASK : 0;
        uint clear_mask = is_cleared ? TILE_META_DATA_CLEAR_MASK : 0;
        uint mask = light_mask | clear_mask;
     
        uint2 DTid = Gid * GroupDim;
        uint groupX = DTid.x >> 3;
        uint groupY = DTid.y >> 3;

        RWTexture2D<uint> g_outMetadata = ResourceDescriptorHeap[g_local.MetadataUAVDescHeapIdx];
        g_outMetadata[uint2(groupX, groupY)] = mask;
    }
}

void FFX_DNSR_Shadows_WriteTemporalCache(uint2 DTid, float2 meanVar)
{
    RWTexture2D<float2> g_outTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalDescHeapIdx];
    g_outTemporalCache[DTid] = meanVar;
}

void FFX_DNSR_Shadows_WriteMoments(uint2 DTid, float3 moments)
{
    RWTexture2D<float3> g_currMoments = ResourceDescriptorHeap[g_local.MomentsUAVHeapIdx];
    g_currMoments[DTid] = moments;
}

bool FFX_DNSR_Shadows_ThreadGroupAllTrue(bool val)
{
    const uint lane_count_in_thread_group = 64;
    if (WaveGetLaneCount() == lane_count_in_thread_group)
    {
        return WaveActiveAllTrue(val);
    }
    else
    {
        GroupMemoryBarrierWithGroupSync();
        g_FFX_DNSR_Shadows_false_count = 0;
        GroupMemoryBarrierWithGroupSync();
        if (!val) 
            g_FFX_DNSR_Shadows_false_count = 1;
        GroupMemoryBarrierWithGroupSync();
        return g_FFX_DNSR_Shadows_false_count == 0;
    }
}

float FFX_DNSR_Shadows_GetLinearDepth(uint2 did, float depth)
{
    return depth;
}

void FFX_DNSR_Shadows_SearchSpatialRegion(uint2 Gid, out bool all_in_light, out bool all_in_shadow)
{
    // The spatial passes can reach a total region of 1+2+4 = 7x7 around each block.
    // The masks are 8x4, so we need a larger vertical stride

    // Visualization - each x represents a 4x4 block, xx is one entire 8x4 mask as read from the raytracer result
    // Same for yy, these are the ones we are working on right now

    // xx xx xx
    // xx xx xx
    // xx yy xx <-- yy here is the base_tile below
    // xx yy xx
    // xx xx xx
    // xx xx xx

    // All of this should result in scalar ops
    const uint2 base_tile = FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(Gid * GroupDim);
    const uint2 numThreadGroups = uint2(g_local.NumShadowMaskThreadGroupsX, g_local.NumShadowMaskThreadGroupsY);
    
    // Load the entire region of masks in a scalar fashion
    uint combined_or_mask = 0;
    uint combined_and_mask = 0xFFFFFFFF;
    for (int j = -2; j <= 3; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            int2 tile_index = base_tile + int2(i, j);
            tile_index = clamp(tile_index, 0, numThreadGroups - 1);
            const uint shadow_mask = FFX_DNSR_Shadows_ReadRaytracedShadowMask(tile_index);

            combined_or_mask = combined_or_mask | shadow_mask;
            combined_and_mask = combined_and_mask & shadow_mask;
        }
    }

    all_in_light = combined_and_mask == 0xFFFFFFFFu;
    all_in_shadow = combined_or_mask == 0u;
}

bool FFX_DNSR_Shadows_IsDisoccluded(uint2 DTid, float depth, float2 velocity)
{
    const float2 currUV = (DTid + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float2 prevUV = currUV - velocity;
    float3 normal = FFX_DNSR_Shadows_ReadNormals(DTid);

    const float currLinearDepth = depth;
    const float3 currPos = Math::WorldPosFromUV(currUV, 
        float2(g_frame.RenderWidth, g_frame.RenderHeight), currLinearDepth, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
        g_frame.CurrCameraJitter);

    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    float prevDepth = g_prevDepth.SampleLevel(g_samPointClamp, prevUV, 0);
    const float3 prevPos = Math::WorldPosFromUV(prevUV, 
        float2(g_frame.RenderWidth, g_frame.RenderHeight), prevDepth, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevViewInv,
        g_frame.PrevCameraJitter);
    
    const float planeDist = dot(normal, prevPos - currPos);
    
    //return planeDist >= g_local.MaxPlaneDist;
    return abs(planeDist) > DISOCCLUSION_TEST_RELATIVE_DELTA * currLinearDepth;
}

float2 FFX_DNSR_Shadows_GetClosestVelocity(int2 DTid, float depth)
{
    float2 closest_velocity = FFX_DNSR_Shadows_ReadVelocity(DTid);
    float closest_depth = depth;

    float new_depth = QuadReadAcrossX(closest_depth);
    float2 new_velocity = QuadReadAcrossX(closest_velocity);

    if (new_depth > closest_depth)
    {
        closest_depth = new_depth;
        closest_velocity = new_velocity;
    }

    new_depth = QuadReadAcrossY(closest_depth);
    new_velocity = QuadReadAcrossY(closest_velocity);

    if (new_depth > closest_depth)
    {
        closest_depth = new_depth;
        closest_velocity = new_velocity;
    }

    //return closest_velocity * float2(0.5f, -0.5f);  // from ndc to uv
    return closest_velocity;
}

float FFX_DNSR_Shadows_KernelWeight(int i)
{
    return KernelMulInvWeightSum[i];      // kernel is symmetric
}

void FFX_DNSR_Shadows_AccumulateMoments(float value, float weight, inout float moments)
{
    // We get value from the horizontal neighborhood calculations. Thus, it's both mean and variance due to using one sample per pixel
    moments += value * weight;
}

// The horizontal part of a 17x17 local neighborhood kernel
float FFX_DNSR_Shadows_HorizontalNeighborhood(int2 DTid)
{
    const int2 base_did = DTid;

    // Prevent vertical out of bounds access
    if ((base_did.y < 0) || (base_did.y >= g_frame.RenderHeight)) 
        return 0;

    const uint2 tile_index = FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(base_did);

    const bool is_first_tile_in_row = tile_index.x == 0;
    const bool is_last_tile_in_row = tile_index.x == (g_local.NumShadowMaskThreadGroupsX - 1);

    uint left_tile = !is_first_tile_in_row ? FFX_DNSR_Shadows_ReadRaytracedShadowMask(uint2(tile_index.x - 1, tile_index.y)) : 0;
    uint center_tile = FFX_DNSR_Shadows_ReadRaytracedShadowMask(tile_index);
    uint right_tile = !is_last_tile_in_row ? FFX_DNSR_Shadows_ReadRaytracedShadowMask(uint2(tile_index.x + 1, tile_index.y)) : 0;

    // Construct a single uint with the lowest 17bits containing the horizontal part of the local neighborhood.

    // First extract the 8 bits of our row in each of the neighboring tiles
    const uint row_base_index = (DTid.y & (4 - 1)) * 8;
    const uint left = (left_tile >> row_base_index) & 0xFF;
    const uint center = (center_tile >> row_base_index) & 0xFF;
    const uint right = (right_tile >> row_base_index) & 0xFF;

    // Combine them into a single mask containting [left, center, right] from least significant to most significant bit
    uint neighborhood = left | (center << 8) | (right << 16);

    // Make sure our pixel is at bit position 9 to get the highest contribution from the filter kernel
    const uint bit_index_in_row = (DTid.x & (8 - 1));
    neighborhood = neighborhood >> bit_index_in_row; // Shift out bits to the right, so the center bit ends up at bit 9.

    float moment = 0.0; // For one sample per pixel this is both, mean and variance

    // First 8 bits up to the center pixel
    uint mask;
    int i;
    for (i = 0; i < 8; ++i)
    {
        mask = 1u << (uint)i;
        moment += (mask & neighborhood) ? FFX_DNSR_Shadows_KernelWeight(8 - i) : 0;
    }

    // Center pixel
    mask = 1u << 8;
    moment += (mask & neighborhood) ? FFX_DNSR_Shadows_KernelWeight(0) : 0;

    // Last 8 bits
    for (i = 1; i <= 8; ++i)
    {
        mask = 1u << (uint)(8 + i);
        moment += (mask & neighborhood) ? FFX_DNSR_Shadows_KernelWeight(i) : 0;
    }

    return moment;
}

float FFX_DNSR_Shadows_ComputeLocalNeighborhood(int2 DTid, int2 GTid)
{
    float local_neighborhood = 0;

    const float upper = FFX_DNSR_Shadows_HorizontalNeighborhood(int2(DTid.x, DTid.y - 8));
    const float center = FFX_DNSR_Shadows_HorizontalNeighborhood(int2(DTid.x, DTid.y));
    const float lower = FFX_DNSR_Shadows_HorizontalNeighborhood(int2(DTid.x, DTid.y + 8));

    g_FFX_DNSR_Shadows_neighborhood[GTid.x][GTid.y] = upper;
    g_FFX_DNSR_Shadows_neighborhood[GTid.x][GTid.y + 8] = center;
    g_FFX_DNSR_Shadows_neighborhood[GTid.x][GTid.y + 16] = lower;

    GroupMemoryBarrierWithGroupSync();

    // First combine the own values.
    // KERNEL_RADIUS pixels up is own upper and KERNEL_RADIUS pixels down is own lower value
    FFX_DNSR_Shadows_AccumulateMoments(center, FFX_DNSR_Shadows_KernelWeight(0), local_neighborhood);
    FFX_DNSR_Shadows_AccumulateMoments(upper, FFX_DNSR_Shadows_KernelWeight(KERNEL_RADIUS), local_neighborhood);
    FFX_DNSR_Shadows_AccumulateMoments(lower, FFX_DNSR_Shadows_KernelWeight(KERNEL_RADIUS), local_neighborhood);

    // Then read the neighboring values.
    for (int i = 1; i < KERNEL_RADIUS; ++i)
    {
        float upper_value = g_FFX_DNSR_Shadows_neighborhood[GTid.x][8 + GTid.y - i];
        float lower_value = g_FFX_DNSR_Shadows_neighborhood[GTid.x][8 + GTid.y + i];
        float weight = FFX_DNSR_Shadows_KernelWeight(i);
        FFX_DNSR_Shadows_AccumulateMoments(upper_value, weight, local_neighborhood);
        FFX_DNSR_Shadows_AccumulateMoments(lower_value, weight, local_neighborhood);
    }

    return local_neighborhood;
}

void FFX_DNSR_Shadows_ClearTargets(uint2 DTid, uint2 GTid, uint2 Gid, float shadow_value, bool is_shadow_receiver, bool all_in_light)
{
    FFX_DNSR_Shadows_WriteTileMetadata(Gid, GTid, true, all_in_light);
    FFX_DNSR_Shadows_WriteTemporalCache(DTid, float2(shadow_value, 0.0f)); // mean, variance

    float temporal_sample_count = is_shadow_receiver ? 1 : 0;
    FFX_DNSR_Shadows_WriteMoments(DTid, float3(shadow_value, 0, temporal_sample_count)); // mean, variance, temporal sample count
}

void FFX_DNSR_Shadows_TileClassification(uint Gidx, uint2 Gid)
{
    const uint2 GTid = FFX_DNSR_Shadows_RemapLane8x8(Gidx); // Make sure we can use the QuadReadAcross intrinsics to access a 2x2 region.
    const uint2 DTid = Gid * GroupDim + GTid;
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float depth = FFX_DNSR_Shadows_ReadDepth(DTid);
    
    const bool is_shadow_receiver = all(DTid.xy < (uint2) renderDim) && (depth != FLT_MAX);
    const bool skip_sky = FFX_DNSR_Shadows_ThreadGroupAllTrue(!is_shadow_receiver);
    
    if (skip_sky)
    {
        // We have to set all resources of the tile we skipped to sensible values as neighboring active denoiser tiles might want to read them.
        FFX_DNSR_Shadows_ClearTargets(DTid, GTid, Gid, 0, is_shadow_receiver, false);
        return;
    }

    bool all_in_light = false;
    bool all_in_shadow = false;
    FFX_DNSR_Shadows_SearchSpatialRegion(Gid, all_in_light, all_in_shadow);

    const float shadow_value = all_in_light ? 1 : 0; 
    // Either all_in_light or all_in_shadow must be true, otherwise we would not skip the tile.
    const bool skip_tile = all_in_light || all_in_shadow;
        
    if (skip_tile)
    {
        // We have to set all resources of the tile we skipped to sensible values as neighboring active denoiser tiles might want to read them.
        FFX_DNSR_Shadows_ClearTargets(DTid, GTid, Gid, shadow_value, is_shadow_receiver, all_in_light);
        return;
    }

    FFX_DNSR_Shadows_WriteTileMetadata(Gid, GTid, false, false);

    const float local_neighborhood = FFX_DNSR_Shadows_ComputeLocalNeighborhood(DTid, GTid);

    const float2 velocity = FFX_DNSR_Shadows_GetClosestVelocity(DTid, depth); // Must happen before we deactivate lanes    
    const float2 uv = (DTid.xy + 0.5f) / renderDim;
    const float2 history_uv = uv - velocity;
    float2 history_pos = history_uv * renderDim;
    history_pos = round(history_pos - 0.5f);    // nearest neighbor
    
    const uint2 tile_index = FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(DTid);
    const uint shadow_tile = FFX_DNSR_Shadows_ReadRaytracedShadowMask(tile_index);

    float3 moments_current = 0;
    float variance = 0;
    float shadow_clamped = 0;
    
    if (is_shadow_receiver) // do not process sky pixels
    {
        const bool hit_light = shadow_tile & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(DTid);
        const float shadow_current = hit_light ? 1.0 : 0.0;

        // Perform moments and variance calculations
        {
            const bool is_disoccluded = FFX_DNSR_Shadows_IsDisoccluded(DTid, depth, velocity);
            const float3 previous_moments = is_disoccluded ? 0.0.xxx    // Can't trust previous moments on disocclusion
                : FFX_DNSR_Shadows_ReadPreviousMomentsBuffer(history_pos);

            const float old_m = previous_moments.x;
            const float old_s = previous_moments.y;
            const float sample_count = previous_moments.z + 1.0f;
            const float new_m = old_m + (shadow_current - old_m) / sample_count;
            const float new_s = old_s + (shadow_current - old_m) * (shadow_current - new_m);

            variance = (sample_count > 1.0f ? new_s / (sample_count - 1.0f) : 1.0f);
            moments_current = float3(new_m, new_s, sample_count);
        }

        // Retrieve local neighborhood and reproject
        {
            float mean = local_neighborhood;
            float spatial_variance = local_neighborhood;
            spatial_variance = max(spatial_variance - mean * mean, 0.0f);
            //spatial_variance /= (17.0f - 1.0f);

            // Compute the clamping bounding box
            const float std_deviation = sqrt(spatial_variance);
            const float nmin = mean - std_deviation;
            const float nmax = mean + std_deviation;

            // Clamp reprojected sample to local neighborhood
            float shadow_previous = shadow_current;
            if (g_local.IsTemporalValid)
                shadow_previous = FFX_DNSR_Shadows_ReadHistory(history_uv);

            shadow_clamped = clamp(shadow_previous, nmin, nmax);

            // Reduce history weighting
#if 0
            const float sigma = 20.0f;
            const float temporal_discontinuity = (shadow_previous - mean) / max(0.5f * std_deviation, 0.001f);
            const float sample_counter_damper = exp(-temporal_discontinuity * temporal_discontinuity / sigma);
            moments_current.z *= sample_counter_damper;
#endif

            // Boost variance on first frames
            if (moments_current.z < 16.0f)
            {
                const float variance_boost = max(16.0f - moments_current.z, 1.0f);
                variance = max(variance, spatial_variance);
                variance *= variance_boost;
            }
        }

        // Perform the temporal blend
        const float history_weight = sqrt(max(32.0f - moments_current.z, 0.0f) / 32.0f);
        shadow_clamped = lerp(shadow_clamped, shadow_current, lerp(0.05f, 1.0f, history_weight));
    }

    // Output the results of the temporal pass 
    FFX_DNSR_Shadows_WriteTemporalCache(DTid, float2(shadow_clamped, variance));
    FFX_DNSR_Shadows_WriteMoments(DTid, moments_current);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(DNSR_TEMPORAL_THREAD_GROUP_SIZE_X, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
    if(!g_local.Denoise)
    {
        const uint2 tile_index = FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(DTid.xy);
        const uint shadow_tile = FFX_DNSR_Shadows_ReadRaytracedShadowMask(tile_index);

        const bool hit_light = shadow_tile & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(DTid.xy);
        const float shadow_current = hit_light ? 1.0 : 0.0;

        RWTexture2D<float> g_outDenoised = ResourceDescriptorHeap[g_local.DenoisedDescHeapIdx];
        g_outDenoised[DTid.xy] = shadow_current;

        return;
    }
    
    FFX_DNSR_Shadows_TileClassification(Gidx, Gid.xy);
}
