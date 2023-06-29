#ifndef RESERVOIR_DI_H
#define RESERVOIR_DI_H

#include "ReSTIR_DI_Common.h"
#include "../Common/Sampling.hlsli"
#include "../Common/BRDF.hlsli"

#define MAX_LUM_VNDF 1e-2

struct DIReservoir
{
	static DIReservoir Init()
	{
		DIReservoir res;
		
		res.Li = 0.0.xxx;
		res.M = 0;
		res.w_sum = 0;
		res.W = 0;
		res.wi = 0.0.xxx;
		res.Target = 0;
		
		return res;
	}
	
	static DIReservoir Init(float w_sum, float W, float M, float3 wi, float3 Li)
	{
		DIReservoir res;
		
		res.Li = Li;
		res.M = M;
		res.w_sum = w_sum;
		res.W = W;
		res.wi = wi;
		res.Target = 0;
		
		return res;
	}
	
	void ComputeW(float maxWSum = FLT_MAX, float m = 0.0f)
	{
		m = m == 0.0f ? 1 / this.M : m;
		// biased but helps with reducing fireflies
		float wSum = min(this.w_sum, maxWSum);
		this.W = this.Target > 0.0 ? (m * wSum) / this.Target : 0.0;
	}
	
	bool Update(float w, float3 Li, float3 wi, float target, inout RNG rng)
	{
		this.w_sum += w;
		this.M += 1;
		
		if (rng.Uniform() < (w / max(1e-6f, this.w_sum)))
		{
			this.wi = wi;
			this.Target = target;
			this.Li = Li;
			
			return true;
		}
		
		return false;
	}
	
	bool Combine(DIReservoir r, float M_max, float weight, float p_q, inout RNG rng)
	{
		float clampedM = min(r.M, M_max);
		float weightedM = clampedM * weight;
		float neighborRISweight = p_q * r.W * weightedM;
		
		float prevM = this.M;
		
		bool updated = Update(neighborRISweight, r.Li, r.wi, p_q, rng);

		this.M = prevM + weightedM;
		
		return updated;
	}
	
	float w_sum;
	float W;
	float M;
	float3 wi;
	float3 Li;
	float Target;
};

namespace RDI_Util
{
	float2 VirtualMotionReproject(float3 posW, float roughness, BRDF::SurfaceInteraction surface, float sampleRayT,
		float k, float linearDepth, float tanHalfFOV, float4x4 prevViewProj)
	{
		float pixelWidth = 2.0 * tanHalfFOV * linearDepth / max(surface.ndotwo, 1e-4);
	
		// curvature computation above uses the opposite signs
		k *= -1.0f;
		k *= pixelWidth;
		float reflectionRayT = sampleRayT / (1.0f - 2 * k * sampleRayT * surface.whdotwo);
	
		// interpolate between virtual motion and surface motion using GGX dominant factor
		// Ref: D. Zhdan, "ReBLUR: A Hierarchical Recurrent Denoiser," in Ray Tracing Gems 2, 2021.
		float factor = BRDF::SpecularBRDFGGXSmithDominantFactor(surface.ndotwo, roughness);
		float3 virtualPos = posW - surface.wo * reflectionRayT * factor;
	
		float4 virtualPosNDC = mul(prevViewProj, float4(virtualPos, 1.0f));
		float2 prevUV = Math::Transform::UVFromNDC(virtualPosNDC.xy / virtualPosNDC.w);

		return prevUV;
	}
	
	DIReservoir ReadReservoir(uint2 DTid, uint inputAIdx, uint inputBIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<float> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];

		const uint4 resA = g_reservoir_A[DTid];
		const float w_sum = g_reservoir_B[DTid];
		const float W = asfloat(resA.w);
		
		uint3 temp1 = uint3(resA.x & 0xffff, 0, resA.x >> 16);
		float3 wi = asfloat16(uint16_t3(temp1));
		float wiNorm = saturate(dot(wi, wi));
		wi.y = wiNorm > 0 ? sqrt(1 - wiNorm) : 1.0f;

		uint3 temp2 = uint3(resA.y & 0xffff, resA.y >> 16, resA.z & 0xffff);
		float3 Li = asfloat16(uint16_t3(temp2));
		float M = asfloat16(uint16_t(resA.z >> 16));
		
		DIReservoir r = DIReservoir::Init(w_sum, W, M, wi, Li);

		return r;
	}
	
	// just wi
	float3 PartialReadReservoir_ReuseWi(uint2 DTid, uint inputAIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		const uint resAx = g_reservoir_A[DTid].x;
		
		uint3 temp1 = uint3(resAx & 0xffff, 0, resAx >> 16);
		float3 wi = asfloat16(uint16_t3(temp1));
		float wiNorm = saturate(dot(wi, wi));
		wi.y = wiNorm > 0 ? sqrt(1 - wiNorm) : 1.0f;
		
		return wi;
	}

	DIReservoir PartialReadReservoir_ReuseRest(uint2 DTid, uint inputAIdx, float3 wi)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		const uint3 resA = g_reservoir_A[DTid].yzw;
		
		const float W = asfloat(resA.z);
		
		uint3 temp2 = uint3(resA.x & 0xffff, resA.x >> 16, resA.y & 0xffff);
		float3 Li = asfloat16(uint16_t3(temp2));
		float M = asfloat16(uint16_t(resA.y >> 16));
		
		DIReservoir r = DIReservoir::Init(0, W, M, wi, Li);

		return r;
	}

	// skips w_sum
	DIReservoir PartialReadReservoir_Shading(uint2 DTid, uint inputAIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];

		const uint4 resA = g_reservoir_A[DTid];
		const float W = asfloat(resA.w);
		
		uint3 temp1 = uint3(resA.x & 0xffff, 0, resA.x >> 16);
		float3 wi = asfloat16(uint16_t3(temp1));
		float wiNorm = saturate(dot(wi, wi));
		wi.y = wiNorm > 0 ? sqrt(1 - wiNorm) : 1.0f;
		
		uint3 temp2 = uint3(resA.y & 0xffff, resA.y >> 16, resA.z & 0xffff);
		float3 Li = asfloat16(uint16_t3(temp2));
		float M = asfloat16(uint16_t(resA.z >> 16));

		DIReservoir r = DIReservoir::Init(0, W, M, wi, Li);

		return r;
	}

	// skips w_sum
	void PartialWriteReservoir(uint2 DTid, DIReservoir r, uint outputAIdx)
	{
		RWTexture2D<uint4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
	
		uint16_t2 wi = asuint16(half2(r.wi.x, r.wi.z));
		uint16_t3 Li = asuint16(half3(r.Li));
		uint16_t M = asuint16(half(r.M));
		
		uint a_x = (uint(wi.y) << 16) | wi.x;
		uint a_y = (uint(Li.y) << 16) | Li.x;
		uint a_z = (uint(M) << 16) | Li.z;
		uint a_w = asuint(r.W);
		
		g_outReservoir_A[DTid] = uint4(a_x, a_y, a_z, a_w);
	}
	
	void WriteReservoir(uint2 DTid, DIReservoir r, uint outputAIdx, uint outputBIdx)
	{
		RWTexture2D<uint4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<float> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];

		uint16_t2 wi = asuint16(half2(r.wi.x, r.wi.z));
		uint16_t3 Li = asuint16(half3(r.Li));
		uint16_t M = asuint16(half(r.M));
		
		uint a_x = (uint(wi.y) << 16) | wi.x;
		uint a_y = (uint(Li.y) << 16) | Li.x;
		uint a_z = (uint(M) << 16) | Li.z;
		uint a_w = asuint(r.W);
		
		g_outReservoir_A[DTid] = uint4(a_x, a_y, a_z, a_w);
		g_outReservoir_B[DTid] = r.w_sum;
	}
}

#endif