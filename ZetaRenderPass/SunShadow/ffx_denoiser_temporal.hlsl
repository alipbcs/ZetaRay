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
//#define KERNEL_WEIGHT(i) (exp(-3.0 * float(i * i) / ((KERNEL_RADIUS + 1.0) * (KERNEL_RADIUS + 1.0))))
//#define KERNEL_WEIGHTS_INV_SUM 0.11082929f

groupshared int g_FFX_DNSR_Shadows_false_count;
groupshared float g_FFX_DNSR_Shadows_neighborhood[8][24];

static const uint2 GroupDim = uint2(DNSR_TEMPORAL_THREAD_GROUP_SIZE_X, DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y);

static const float KernelMulInvWeightSum[9] =
{
	0.012283131088539638,
	0.011836521899571278,
	0.01059178517007069,
	0.008801248016820962,
	0.006791244929512557,
	0.004866139629771234,
	0.0032377982020393606,
	0.0020005298091230485,
	0.0011478108212715531
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
	const float3 normal = Math::Encoding::DecodeUnitNormal(g_normal[DTid]);

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
    
	if (!Math::IsWithinBoundsExc(history_pos, int2(g_frame.RenderWidth, g_frame.RenderHeight)))
		return 0.0.xxx;
        
	return g_prevMoments[history_pos];
}

float FFX_DNSR_Shadows_ReadHistory(float2 histUV)
{
	Texture2D<float2> g_prevTemporalCache = ResourceDescriptorHeap[g_local.PrevTemporalCacheHeapIdx];
	return g_prevTemporalCache.SampleLevel(g_samPointClamp, histUV, 0).x;
}

void FFX_DNSR_Shadows_WriteTileMetadata(uint2 Gid, uint2 GTid, bool is_cleared, bool all_in_light)
{
	if (all(GTid == 0))
	{
		uint light_mask = all_in_light ? TILE_META_DATA_LIGHT_MASK : 0;
		uint clear_mask = is_cleared ? TILE_META_DATA_CLEAR_MASK : 0;
		uint mask = light_mask | clear_mask;
        
		RWTexture2D<uint> g_outMetadata = ResourceDescriptorHeap[g_local.MetadataUAVDescHeapIdx];
		g_outMetadata[Gid] = mask;
	}
}

void FFX_DNSR_Shadows_WriteTemporalCache(uint2 DTid, float2 meanVar)
{
	RWTexture2D<float2> g_outTemporalCache = ResourceDescriptorHeap[g_local.CurrTemporalCacheHeapIdx];
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
#if 0
    const float2 uv = (did + 0.5f) * data.InvBufferDim;
    const float2 ndc = 2.0f * float2(uv.x, 1.0f - uv.y) - 1.0f;

    float4 projected = mul(FFX_DNSR_Shadows_GetProjectionInverse(), float4(ndc, depth, 1));
    return abs(projected.z / projected.w);
#endif
    
	return Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
}

void FFX_DNSR_Shadows_SearchSpatialRegion(uint2 gid, out bool all_in_light, out bool all_in_shadow)
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
    const uint2 base_tile = FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(gid * GroupDim);
	const uint2 numThreadGroups = uint2(g_local.NumShadowMaskThreadGroupsX, g_local.NumShadowMaskThreadGroupsY);
    
    // Load the entire region of masks in a scalar fashion
    uint combined_or_mask = 0;
    uint combined_and_mask = 0xFFFFFFFF;
    for (int j = -2; j <= 3; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            int2 tile_index = base_tile + int2(i, j);
			tile_index = clamp(tile_index, 0, numThreadGroups);
			//const uint linear_tile_index = FFX_DNSR_Shadows_LinearTileIndex(tile_index, data.BufferDim.x);
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
	const float2 dims = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 uv = (DTid + 0.5f) / dims;
    //const float2 ndc = (2.0f * uv - 1.0f) * float2(1.0f, -1.0f);
    const float2 previous_uv = uv - velocity;
	const float currlinearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
    
    bool is_disoccluded = true;
    if (all(previous_uv > 0.0) && all(previous_uv < 1.0))
    {
        // Read the center values
		float3 normal = FFX_DNSR_Shadows_ReadNormals(DTid);

		const float3 world_position = Math::Transform::WorldPosFromUV(uv,
            currlinearDepth,
            g_frame.TanHalfFOV,
            g_frame.AspectRatio,
            g_frame.CurrViewInv);

        //float4 clip_space = mul(FFX_DNSR_Shadows_GetReprojectionMatrix(), float4(ndc, depth, 1.0f));
		float4 clip_space = mul(float4(world_position, 1.0f), g_frame.PrevViewProj);
        clip_space /= clip_space.w; // perspective divide

        // How aligned with the view vector? (the more Z aligned, the higher the depth errors)
        //const float4 homogeneous = mul(FFX_DNSR_Shadows_GetViewProjectionInverse(), float4(ndc, depth, 1.0f));
        //const float3 world_position = homogeneous.xyz / homogeneous.w;  // perspective divide        

        const float3 view_direction = normalize(g_frame.CameraPos.xyz - world_position);
        float z_alignment = 1.0f - dot(view_direction, normal);
        z_alignment = pow(z_alignment, 8);

        // Calculate the depth difference
		float linear_depth = FFX_DNSR_Shadows_GetLinearDepth(DTid, clip_space.z); // get linear depth

        int2 idx = previous_uv * dims;
        const float previous_depth = FFX_DNSR_Shadows_GetLinearDepth(idx, FFX_DNSR_Shadows_ReadPreviousDepth(idx));
        const float depth_difference = abs(previous_depth - linear_depth) / linear_depth;

        // Resolve into the disocclusion mask
        const float depth_tolerance = lerp(1e-2f, 1e-1f, z_alignment);
        is_disoccluded = depth_difference >= depth_tolerance;
    }

    return is_disoccluded;
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
    // Statically initialize kernel_weights_sum
#if 0
    float kernel_weights_sum = 0;
    kernel_weights_sum += KERNEL_WEIGHT(0);
    for (int c = 1; c <= KERNEL_RADIUS; ++c)
    {
        kernel_weights_sum += 2 * KERNEL_WEIGHT(c); // Add other half of the kernel to the sum
    }
    float inv_kernel_weights_sum = rcp(kernel_weights_sum);

    // The only runtime code in this function
    return KERNEL_WEIGHT(i) * inv_kernel_weights_sum;
#endif

	//return Kernel[int(i)] * KERNEL_WEIGHTS_INV_SUM;
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
        mask = 1u << i;
        moment += (mask & neighborhood) ? FFX_DNSR_Shadows_KernelWeight(8 - i) : 0;
    }

    // Center pixel
    mask = 1u << 8;
    moment += (mask & neighborhood) ? FFX_DNSR_Shadows_KernelWeight(0) : 0;

    // Last 8 bits
    for (i = 1; i <= 8; ++i)
    {
        mask = 1u << (8 + i);
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
	FFX_DNSR_Shadows_WriteTemporalCache(DTid, float2(shadow_value, 0)); // mean, variance

    float temporal_sample_count = is_shadow_receiver ? 1 : 0;
	FFX_DNSR_Shadows_WriteMoments(DTid, float3(shadow_value, 0, temporal_sample_count)); // mean, variance, temporal sample count
}

void FFX_DNSR_Shadows_TileClassification(uint Gidx, uint2 Gid, bool isShadowReceiver)
{
	const uint2 GTid = FFX_DNSR_Shadows_RemapLane8x8(Gidx); // Make sure we can use the QuadReadAcross intrinsics to access a 2x2 region.
	const uint2 DTid = Gid * GroupDim + GTid;

	const bool skip_sky = FFX_DNSR_Shadows_ThreadGroupAllTrue(!isShadowReceiver);
    if (skip_sky)
    {
        // We have to set all resources of the tile we skipped to sensible values as neighboring active denoiser tiles might want to read them.
		FFX_DNSR_Shadows_ClearTargets(DTid, GTid, Gid, 0, isShadowReceiver, false);
        return;
    }

    bool all_in_light = false;
    bool all_in_shadow = false;
	FFX_DNSR_Shadows_SearchSpatialRegion(Gid, all_in_light, all_in_shadow);

    const float shadow_value = all_in_light ? 1 : 0; 
    // Either all_in_light or all_in_shadow must be true, otherwise we would not skip the tile.
    const bool can_skip = all_in_light || all_in_shadow;
    // We have to append the entire tile if there is a single lane that we can't skip
    //const bool skip_tile = FFX_DNSR_Shadows_ThreadGroupAllTrue(can_skip);
	const bool skip_tile = can_skip;
        
    if (skip_tile)
    {
        // We have to set all resources of the tile we skipped to sensible values as neighboring active denoiser tiles might want to read them.
		FFX_DNSR_Shadows_ClearTargets(DTid, GTid, Gid, shadow_value, isShadowReceiver, all_in_light);
        return;
    }

	FFX_DNSR_Shadows_WriteTileMetadata(Gid, GTid, false, false);

	const float local_neighborhood = FFX_DNSR_Shadows_ComputeLocalNeighborhood(DTid, GTid);

	const float depth = FFX_DNSR_Shadows_ReadDepth(DTid);
	const float2 velocity = FFX_DNSR_Shadows_GetClosestVelocity(DTid, depth); // Must happen before we deactivate lanes	
    const float2 dim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float2 uv = (DTid.xy + 0.5f) / dim;
    const float2 history_uv = uv - velocity;
	float2 history_pos = history_uv * dim;
	history_pos = round(history_pos - 0.5f);    // nearest neighbor
    
	const uint2 tile_index = FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(DTid);
	const uint shadow_tile = FFX_DNSR_Shadows_ReadRaytracedShadowMask(tile_index);

    float3 moments_current = 0;
    float variance = 0;
    float shadow_clamped = 0;
    
	if (isShadowReceiver) // do not process sky pixels
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
            const float nmin = mean - 0.5f * std_deviation;
            const float nmax = mean + 0.5f * std_deviation;

            // Clamp reprojected sample to local neighborhood
            float shadow_previous = shadow_current;
			if (g_local.IsTemporalValid)
                shadow_previous = FFX_DNSR_Shadows_ReadHistory(history_uv);

            shadow_clamped = clamp(shadow_previous, nmin, nmax);

            // Reduce history weighting
			const float sigma = 20.0f;
			const float temporal_discontinuity = (shadow_previous - mean) / max(0.5f * std_deviation, 0.001f);
			const float sample_counter_damper = exp(-temporal_discontinuity * temporal_discontinuity / sigma);
            moments_current.z *= sample_counter_damper;

            // Boost variance on first frames
            if (moments_current.z < 16.0f)
            {
                const float variance_boost = max(16.0f - moments_current.z, 1.0f);
                variance = max(variance, spatial_variance);
                variance *= variance_boost;
            }
        }

        // Perform the temporal blend
        const float history_weight = sqrt(max(8.0f - moments_current.z, 0.0f) / 8.0f);
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
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	if (!Math::IsWithinBoundsExc(DTid.xy, renderDim))
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];
	const bool isShadowReceiver = depth > 0.0;
    
	FFX_DNSR_Shadows_TileClassification(Gidx, Gid.xy, isShadowReceiver);
}
