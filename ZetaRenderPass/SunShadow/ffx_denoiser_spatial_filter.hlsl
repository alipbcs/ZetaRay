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
#include "SunShadow_Common.h"

#define KERNEL_RADIUS 1
#define DISOCCLUSION_TEST_RELATIVE_DELTA 0.005f

static const unsigned int Width = 2 * KERNEL_RADIUS + 1;
static const float Kernel1D[Width] = { 0.27901, 0.44198, 0.27901 };
static const float Kernel[Width][Width] =
{
	{ Kernel1D[0] * Kernel1D[0], Kernel1D[0] * Kernel1D[1], Kernel1D[0] * Kernel1D[2] },
	{ Kernel1D[1] * Kernel1D[0], Kernel1D[1] * Kernel1D[1], Kernel1D[1] * Kernel1D[2] },
	{ Kernel1D[2] * Kernel1D[0], Kernel1D[2] * Kernel1D[1], Kernel1D[2] * Kernel1D[2] },
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbFFX_DNSR_Spatial> g_local : register(b1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

groupshared half2 g_FFX_DNSR_shared_input[16][16];
groupshared float g_FFX_DNSR_shared_depth[16][16];
groupshared float2 g_FFX_DNSR_shared_normal[16][16];

void FFX_DNSR_Shadows_ReadTileMetadata(uint2 Gid, out bool is_cleared, out bool all_in_light)
{
	Texture2D<uint> g_metadata = ResourceDescriptorHeap[g_local.MetadataSRVDescHeapIdx];
	uint meta_data = g_metadata[Gid];
    
	is_cleared = meta_data & TILE_META_DATA_CLEAR_MASK;
	all_in_light = meta_data & TILE_META_DATA_LIGHT_MASK;
}

void FFX_DNSR_Shadows_WriteTemporalCache(uint2 DTid, float2 meanVar)
{
	RWTexture2D<float2> g_outTemporalCache = ResourceDescriptorHeap[g_local.OutTemporalDescHeapIdx];
	g_outTemporalCache[DTid] = meanVar;
}

void FFX_DNSR_Shadows_LoadWithOffset(int2 DTid, int2 offset, out float2 normal, out half2 input, out float depth)
{
    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	Texture2D<half2> g_temporalCache = ResourceDescriptorHeap[g_local.InTemporalDescHeapIdx];

	DTid += offset;
	const int2 p = clamp(DTid, 0.0.xx, int2(g_frame.RenderWidth, g_frame.RenderHeight) - 1);
    
	depth = g_depth[p];
	normal = g_normal[p];
	input = g_temporalCache[p];
}

void FFX_DNSR_Shadows_StoreWithOffsetInGroupSharedMemory(int2 GTid, int2 offset, float2 normal, half2 input, float depth)
{
	GTid += offset;
    
	g_FFX_DNSR_shared_depth[GTid.y][GTid.x] = depth;
	g_FFX_DNSR_shared_normal[GTid.y][GTid.x] = normal;
	g_FFX_DNSR_shared_input[GTid.y][GTid.x] = input;
}

void FFX_DNSR_Shadows_InitializeGroupSharedMemory(int2 DTid, int2 GTid)
{
	int2 offset_0 = 0;
	int2 offset_1 = int2(8, 0);
	int2 offset_2 = int2(0, 8);
	int2 offset_3 = int2(8, 8);

	float2 normals_0;
	half2 input_0;
	float depth_0;

	float2 normals_1;
	half2 input_1;
	float depth_1;

	float2 normals_2;
	half2 input_2;
	float depth_2;

	float2 normals_3;
	half2 input_3;
	float depth_3;

    /// XA
    /// BC

	DTid -= 4;
	FFX_DNSR_Shadows_LoadWithOffset(DTid, offset_0, normals_0, input_0, depth_0); // X
	FFX_DNSR_Shadows_LoadWithOffset(DTid, offset_1, normals_1, input_1, depth_1); // A
	FFX_DNSR_Shadows_LoadWithOffset(DTid, offset_2, normals_2, input_2, depth_2); // B
	FFX_DNSR_Shadows_LoadWithOffset(DTid, offset_3, normals_3, input_3, depth_3); // C

	FFX_DNSR_Shadows_StoreWithOffsetInGroupSharedMemory(GTid, offset_0, normals_0, input_0, depth_0); // X
	FFX_DNSR_Shadows_StoreWithOffsetInGroupSharedMemory(GTid, offset_1, normals_1, input_1, depth_1); // A
	FFX_DNSR_Shadows_StoreWithOffsetInGroupSharedMemory(GTid, offset_2, normals_2, input_2, depth_2); // B
	FFX_DNSR_Shadows_StoreWithOffsetInGroupSharedMemory(GTid, offset_3, normals_3, input_3, depth_3); // C
}

float FFX_DNSR_Shadows_GetShadowSimilarity(float x1, float x2, float sigma)
{
	return exp(-abs(x1 - x2) / (sigma * g_local.EdgeStoppingShadowStdScale));
}

float FFX_DNSR_Shadows_GetDepthSimilarity(float3 samplePos, float3 currNormal, float3 currPos, float currLinearDepth)
{
	float planeDist = dot(currNormal, samplePos - currPos);	
	float weight = abs(planeDist) <= DISOCCLUSION_TEST_RELATIVE_DELTA * currLinearDepth;
	
	return weight;
}

float FFX_DNSR_Shadows_GetNormalSimilarity(float3 x1, float3 x2, float p)
{
	return pow(saturate(dot(x1, x2)), p);
}

float FFX_DNSR_Shadows_FilterVariance(int2 pos)
{
	const int k = 1;
	float variance = 0.0f;
	const float kernel[2][2] =
	{
		{ 1.0f / 4.0f, 1.0f / 8.0f },
		{ 1.0f / 8.0f, 1.0f / 16.0f }
	};
    
	for (int y = -k; y <= k; ++y)
	{
		for (int x = -k; x <= k; ++x)
		{
			const float w = kernel[abs(x)][abs(y)];
			int2 neighborPos = pos + int2(x, y);
			variance += w * g_FFX_DNSR_shared_input[neighborPos.y][neighborPos.x].y;
		}
	}

	return variance;
}

void FFX_DNSR_Shadows_DenoiseFromGroupSharedMemory(uint2 DTid, uint2 GTid, float depth, uint stepsize,
    inout float weight_sum, inout float2 shadow_sum)
{
    // Load our center sample
	const float2 shadow_center = g_FFX_DNSR_shared_input[GTid.y][GTid.x];
	const float3 currNormal = Math::Encoding::DecodeUnitVector(g_FFX_DNSR_shared_normal[GTid.y][GTid.x]);

	weight_sum = 1.0f;
	shadow_sum = shadow_center;

	const float variance = max(FFX_DNSR_Shadows_FilterVariance(GTid), g_local.MinFilterVar);
	const float std_deviation = sqrt(max(variance + 1e-9f, 0.0f));
#if RT_GBUFFER == 1
	const float currLinearDepth = depth;
#else
	const float currLinearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
#endif
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 currPos = Math::Transform::WorldPosFromScreenSpace(DTid, 
        renderDim,
        currLinearDepth,
        g_frame.TanHalfFOV, 
        g_frame.AspectRatio, 
        g_frame.CurrViewInv,
		g_frame.CurrCameraJitter);
    
    // Iterate filter kernel
	for (int y = -KERNEL_RADIUS; y <= KERNEL_RADIUS; ++y)
	{
		for (int x = -KERNEL_RADIUS; x <= KERNEL_RADIUS; ++x)
		{
            // Should we process this sample?
			const int2 step = int2(x, y) * stepsize;
			const int2 gtid_idx = GTid + step;
			const int2 did_idx = DTid + step;

			float neighborDepth = g_FFX_DNSR_shared_depth[gtid_idx.y][gtid_idx.x];
			float3 neighborNormal = Math::Encoding::DecodeUnitVector(g_FFX_DNSR_shared_normal[gtid_idx.y][gtid_idx.x]);
			float2 shadow_neigh = g_FFX_DNSR_shared_input[gtid_idx.y][gtid_idx.x];

			float sky_pixel_multiplier = ((x == 0 && y == 0) || neighborDepth == FLT_MAX) ? 0 : 1; // Zero weight for sky pixels

            // Fetch our filtering values
#if RT_GBUFFER == 0
			neighborDepth = Math::Transform::LinearDepthFromNDC(neighborDepth, g_frame.CameraNear);
#endif
			float3 posNeighbor = Math::Transform::WorldPosFromScreenSpace(did_idx,
                renderDim,
                neighborDepth,
                g_frame.TanHalfFOV,
                g_frame.AspectRatio,
                g_frame.CurrViewInv,
				g_frame.CurrCameraJitter);
            
            // Evaluate the edge-stopping function
			float w = Kernel[abs(y)][abs(x)]; // kernel weight
			w *= FFX_DNSR_Shadows_GetShadowSimilarity(shadow_center.x, shadow_neigh.x, std_deviation);
			w *= FFX_DNSR_Shadows_GetDepthSimilarity(posNeighbor, currNormal, currPos, currLinearDepth);
			w *= FFX_DNSR_Shadows_GetNormalSimilarity(currNormal, neighborNormal, g_local.EdgeStoppingNormalExp);
			w *= sky_pixel_multiplier;

            // Accumulate the filtered sample
			shadow_sum += float2(w, w * w) * shadow_neigh;
			weight_sum += w;
		}
	}
}

float2 FFX_DNSR_Shadows_ApplyFilterWithPrecache(uint2 DTid, uint2 GTid, uint stepsize, float depth, bool isShadowReceiver)
{
	float weight_sum = 1.0;
	float2 shadow_sum = 0.0;

	FFX_DNSR_Shadows_InitializeGroupSharedMemory(DTid, GTid);
    
	GroupMemoryBarrierWithGroupSync();
    
	if (isShadowReceiver)
	{
		GTid += 4; // Center threads in groupshared memory
		FFX_DNSR_Shadows_DenoiseFromGroupSharedMemory(DTid, GTid, depth, stepsize, weight_sum, shadow_sum);
	}

	float mean = shadow_sum.x / weight_sum;
	float variance = shadow_sum.y / (weight_sum * weight_sum);
	return float2(mean, variance);
}

float2 FFX_DNSR_Shadows_FilterSoftShadowsPass(uint2 Gid, uint2 GTid, uint2 DTid, uint passNum, uint stepsize,
    float depth, bool isShadowReceiver, out bool writeResults)
{
	bool is_cleared;
	bool all_in_light;
	FFX_DNSR_Shadows_ReadTileMetadata(Gid, is_cleared, all_in_light);

	writeResults = false;
	float2 results = 0.0.xx;
    
    [branch]
	if (is_cleared)
	{
		//if (passNum != 0)
		{
			results.x = all_in_light ? 1.0 : 0.0;
			writeResults = true;
		}
	}
	else
	{
		results = FFX_DNSR_Shadows_ApplyFilterWithPrecache(DTid, GTid, stepsize, depth, isShadowReceiver);
		writeResults = true;
	}

	return results;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_X, DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
	if(DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
		return;
	
    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[DTid.xy];
	const bool isShadowReceiver = depth != FLT_MAX;

	bool writeResutls;
	float2 results = FFX_DNSR_Shadows_FilterSoftShadowsPass(Gid.xy, GTid.xy, DTid.xy, g_local.PassNum, g_local.StepSize,
        depth, isShadowReceiver, writeResutls);

	if (writeResutls)
		FFX_DNSR_Shadows_WriteTemporalCache(DTid.xy, results);
	
	if(g_local.WriteDenoised)
	{
		RWTexture2D<float> g_outDenoised = ResourceDescriptorHeap[g_local.DenoisedDescHeapIdx];
		g_outDenoised[DTid.xy] = results.x;
	}
}
