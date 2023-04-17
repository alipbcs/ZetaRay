// Ref: https://interplayoflight.wordpress.com/2017/11/15/experiments-in-gpu-based-occlusion-culling/

#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "GBuffer_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbOcclussionCulling> g_local : register(b1);
StructuredBuffer<MeshInstance> g_meshes : register(t0);
RWByteAddressBuffer g_indirectDrawArgs : register(u0);
RWByteAddressBuffer g_visibility : register(u1);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool LoadVisibility(uint visIdx, inout uint byteOffset)
{
	byteOffset = (visIdx >> 5) * sizeof(uint);
	const uint visibilityLastFrame = g_visibility.Load(byteOffset);	
	return (visibilityLastFrame & (1u << (visIdx & 31))) != 0;
}

void FlipVisibility(uint visIdx, uint byteOffset)
{
	// flip the respective bit
	const uint mask = 1u << (visIdx & 31);
	g_visibility.InterlockedXor(byteOffset, mask);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(BUILD_OCC_CULL_THREAD_GROUP_SIZE_X, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= g_local.NumMeshes)
		return;
	
	const uint meshBuffIdx = g_local.MeshBufferStartIndex + DTid.x;
	const uint visIdx = g_meshes[meshBuffIdx].VisibilityIdx;
	
	uint visByteOffset;
	const bool wasVisibleLastFrame = LoadVisibility(visIdx, visByteOffset);
	
	const float3 center = g_meshes[meshBuffIdx].BoundingBox.Center;
	const float3 ext = g_meshes[meshBuffIdx].BoundingBox.Extents;
	
	float3 cornerOffsets[8] =
	{
		-ext,
		float3(-ext.x, -ext.y, ext.z),
		float3(-ext.x, ext.y, -ext.z),
		float3(-ext.x, ext.y, ext.z),
		float3(ext.x, -ext.y, -ext.z),
		float3(ext.x, -ext.y, ext.z),
		float3(ext.x, ext.y, -ext.z),
		ext
	};
	
	float2 minPosUV = 1.xx;
	float2 maxPosUV = 0.xx;
	float maxZ_NDC = 0;
	float3 nearestCorner = -FLT_MAX.xxx;
	
	[unroll]
	for (int i = 0; i < 8; i++)
	{
		float3 boxCorner = center + cornerOffsets[i];
		float4 posH = mul(g_local.ViewProj, float4(boxCorner, 1.0f));
		posH.xyz /= posH.w;
		posH.xy = clamp(posH.xy, -1, 1);
		
		float2 uv = Math::Transform::UVFromNDC(posH.xy);
		minPosUV = min(minPosUV, uv);
		maxPosUV = max(maxPosUV, uv);
		
		if (posH.z > maxZ_NDC)
		{
			maxZ_NDC = posH.z;
			nearestCorner = boxCorner;
		}
	}
	
#if 0	
	const float2 aabbScreen = (maxPosUV - minPosUV) * float2(g_local.DepthPyramidMip0DimX, g_local.DepthPyramidMip0DimY);
	float mip = ceil(log2(max(aabbScreen.x, aabbScreen.y)));
#else
	const float2 aabbScreen = (maxPosUV - minPosUV) * float2(g_frame.RenderWidth, g_frame.RenderHeight);
	float mip = max(ceil(log2(max(aabbScreen.x, aabbScreen.y))) - 1, 0);
#endif
	
	float prevMip = max(mip - 1, 0);
	float2 scale = exp2(-prevMip);
	float2 a = floor(minPosUV * scale);
	float2 b = ceil(maxPosUV * scale);
	float2 dims = b - a;
 
	if (dims.x <= 2 && dims.y <= 2)
		mip = prevMip;
	
	float2 aabbCornersScreen[4];
	aabbCornersScreen[0] = minPosUV;
	aabbCornersScreen[1] = maxPosUV;
	aabbCornersScreen[2] = float2(minPosUV.x, maxPosUV.y);
	aabbCornersScreen[3] = float2(maxPosUV.x, minPosUV.y);
	
	Texture2D<float> g_depthPyramid = ResourceDescriptorHeap[g_local.DepthPyramidSrvDescHeapIdx];
	float minFootprintZ = FLT_MAX;
	
	[unroll]
	for (int j = 0; j < 4; j++)
	{
		float sampleZ = g_depthPyramid.SampleLevel(g_samMinPointClamp, aabbCornersScreen[j], mip);
		minFootprintZ = min(minFootprintZ, sampleZ);
	}

	// attempt to handle the corner case
#if 1
	const float3 cameraBasisZ = float3(g_frame.CurrViewInv._m02, g_frame.CurrViewInv._m12, g_frame.CurrViewInv._m22);
	const bool cornerCase = (dot(cameraBasisZ, normalize(nearestCorner - g_frame.CameraPos)) <= 1e-5f) && 
		(mip >= g_local.NumDepthPyramidMips - 3);
	const bool isVisible = (minFootprintZ <= maxZ_NDC + g_local.DepthThresh) || cornerCase;
#else
	bool isVisible = (minFootprintZ <= maxZ + 1e-3f);
#endif
	
	const bool needsDraw = !wasVisibleLastFrame && isVisible;
	const uint numToDrawInWave = WaveActiveCountBits(needsDraw);
	
	uint baseIdxToStore = 0;
	
	if (numToDrawInWave > 0 && WaveIsFirstLane())
		g_indirectDrawArgs.InterlockedAdd(g_local.CounterBufferOffset, numToDrawInWave, baseIdxToStore);

	// broadcast base index to all lanes
	baseIdxToStore = WaveReadLaneFirst(baseIdxToStore);
	const uint laneIdxToStore = baseIdxToStore + WavePrefixSum(needsDraw);
	
	if (needsDraw)
	{
		CommandSig cmdSig;
		cmdSig.RootConstant = meshBuffIdx;

		cmdSig.DrawArgs.IndexCountPerInstance = g_meshes[meshBuffIdx].IndexCount;
		cmdSig.DrawArgs.InstanceCount = 1;
		cmdSig.DrawArgs.StartIndexLocation = g_meshes[meshBuffIdx].BaseIdxOffset;
		cmdSig.DrawArgs.BaseVertexLocation = 0;
		cmdSig.DrawArgs.StartInstanceLocation = 0;

		const uint byteOffsetToStore = g_local.ArgBufferStartOffsetInBytes + laneIdxToStore * sizeof(CommandSig);
		g_indirectDrawArgs.Store<CommandSig>(byteOffsetToStore, cmdSig);
	}
	
	// update the visibility
	if (wasVisibleLastFrame != isVisible)
		FlipVisibility(visIdx, visByteOffset);
}
