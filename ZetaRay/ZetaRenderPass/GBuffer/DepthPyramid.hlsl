#include "GBuffer_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"

#define A_GPU 1
#define A_HLSL 1
#include "../Common/ffx_a.h"

groupshared AF1 spdIntermediate[16][16];
groupshared AU1 spdCounter;

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbDepthPyramid> g_local : register(b1);
globallycoherent RWStructuredBuffer<uint> g_spdGlobalAtomic : register(u2);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
	GBUFFER_DEPTH g_currDepth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const int2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	//p = clamp(p, 0, renderDim - 1);
	bool inScreen = p.x >= 0 && p.x < g_frame.RenderWidth && p.y >= 0 && p.y < g_frame.RenderHeight;
	float depth = inScreen ? g_currDepth[p] : 0.0;
		
	return depth.xxxx;
}

AF4 SpdLoad(ASU2 p, AU1 slice)
{
	// load from output MIP 5
	const uint heapIdx = g_local.Mips4_7[1];
	globallycoherent RWTexture2D<float> g_mip5 = ResourceDescriptorHeap[heapIdx];
	p = clamp(p, 0, float2(g_local.Mip5DimX, g_local.Mip5DimY) - 1);
	float val = g_mip5[p];
	
	return val.xxxx;
} 

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{	
	uint4 mipGroup;
	
	if(mip < 4)
		mipGroup = g_local.Mips0_3;
	else if(mip < 8)
		mipGroup = g_local.Mips4_7;
	else
		mipGroup = g_local.Mips8_11;
	
	const uint offset = mip & 3;
	const uint heapIdx = NonUniformResourceIndex(mipGroup[offset]);
	
	globallycoherent RWTexture2D<float> g_out = ResourceDescriptorHeap[heapIdx];
	g_out[p] = value.x;
}

void SpdIncreaseAtomicCounter(AU1 slice)
{
	InterlockedAdd(g_spdGlobalAtomic[0], 1, spdCounter);
}

AU1 SpdGetAtomicCounter()
{
	return spdCounter;
}

void SpdResetAtomicCounter(AU1 slice)
{
	g_spdGlobalAtomic[0] = 0;
}

AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
	return spdIntermediate[x][y].xxxx;
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
	spdIntermediate[x][y] = value.x;
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
	return min(min(v0.x, v1.x), min(v2.x, v3.x)).xxxx;
}

//--------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------

#include "../Common/ffx_spd.h"

[numthreads(256, 1, 1)]
void main(uint Gidx : SV_GroupIndex, uint3 Gid : SV_GroupID)
{
	SpdDownsample(Gid.xy,
		Gidx,
		g_local.MipLevels, 
		g_local.NumThreadGroupsX * g_local.NumThreadGroupsY, 
		0);
}