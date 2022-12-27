// Ref: https://github.com/GPUOpen-Effects/FidelityFX-FSR2

#include "AutoExposure_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"

//#define SPD_LINEAR_SAMPLER
#define A_GPU 1
#define A_HLSL 1
#include "../Common/ffx_a.h"

groupshared AF1 spdIntermediate[16][16];
groupshared AU1 spdCounter;

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbAutoExposure> g_local : register(b1);
globallycoherent RWStructuredBuffer<uint> g_spdGlobalAtomic : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float ComputeAutoExposureFromLogAverage(float averageLogLuminance)
{
	const float averageLuminance = exp(averageLogLuminance);
	const float S = 100.0f; // ISO arithmetic speed
	const float K = 12.5f;
	const float exposureIso100 = log2((averageLuminance * S) / K);
	const float q = 0.65f;
	const float luminanceMax = (78.0f / (q * S)) * pow(2.0f, exposureIso100);
	return 1 / luminanceMax;
}

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
	Texture2D<half4> g_input = ResourceDescriptorHeap[g_local.InputDescHeapIdx];
	const int2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const bool isWithinBounds = Math::IsWithinBoundsExc(p, renderDim);
	float3 color = isWithinBounds ? g_input[p].rgb : 0.0.xxx;
	float lum = Math::Color::LuminanceFromLinearRGB(color);
	
	if (g_local.ClampLum)
		lum = clamp(lum, g_local.MinLum, g_local.MaxLum);
		
	float logLum = isWithinBounds ? log(max(lum, 1e-3f)) : 0.0f;
		
	return float4(logLum, 0, 0, 0);
}

AF4 SpdLoad(ASU2 p, AU1 slice)
{
	// load from output MIP 5
	globallycoherent RWTexture2D<float> g_mip5 = ResourceDescriptorHeap[g_local.OutputMip5DescHeapIdx];
	float val = g_mip5[p];
	
	return float4(val, 0, 0, 0);
} 

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{	
	if(mip == 5)
	{
		if (Math::IsWithinBoundsExc(p, int2(g_local.Mip5DimX, g_local.Mip5DimY)))
		{
			globallycoherent RWTexture2D<float> g_mip5 = ResourceDescriptorHeap[g_local.OutputMip5DescHeapIdx];
			g_mip5[p] = value.r;
		}
	}
	else if(mip == g_local.MipLevels - 1)
	{
		if (all(p == 0))
		{
			globallycoherent RWTexture2D<float2> g_out = ResourceDescriptorHeap[g_local.OutputLastMipDescHeapIdx];
			float prev = g_out[int2(0, 0)].y;
			float result = value.r / (g_frame.RenderWidth * g_frame.RenderHeight);

			if (prev < 1e8f)
			{
				float rate = 1.0f;
				result = prev + (result - prev) * (1 - exp(-g_frame.dt * 1000.0f * rate));
			}
			
			float exposure = ComputeAutoExposureFromLogAverage(result);
			g_out[int2(0, 0)] = float2(exposure, result);
		}
	}
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
	return v0 + v1 + v2 + v3;
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