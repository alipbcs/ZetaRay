#ifndef SKY_DI_H
#define SKY_DI_H

#include "SkyDI_Common.h"
#include "../../Common/Sampling.hlsli"
#include "../../Common/BRDF.hlsli"

namespace SkyDI_Util
{
	struct Reservoir
	{
		static Reservoir Init()
		{
			Reservoir res;
		
			res.Le = 0.0.xxx;
			res.M = 0;
			res.w_sum = 0;
			res.W = 0;
			res.wi = 0.0.xxx;
			res.Target = 0;
			res.NeedsShadowRay = false;
			res.Visible = true;
		
			return res;
		}
	
		static Reservoir Init(float w_sum, float W, half M, float3 wi, float3 le)
		{
			Reservoir res;
		
			res.Le = le;
			res.M = M;
			res.w_sum = w_sum;
			res.W = W;
			res.wi = wi;
			res.Target = 0;
			res.NeedsShadowRay = false;
			res.Visible = true;
		
			return res;
		}
	
		bool IsValid()
		{
			return !all(this.wi == 0);
		}

		bool Update(float w, float3 le, float3 wi, float3 target, inout RNG rng)
		{
			this.w_sum += w;
			this.M += 1;
		
			if (rng.Uniform() < (w / max(1e-6f, this.w_sum)))
			{
				this.wi = wi;
				this.Target = target;
				this.Le = le;
			
				return true;
			}
		
			return false;
		}
	
		float w_sum;
		float W;
		float3 wi;
		float3 Le;
		float3 Target;
		half M;
		bool NeedsShadowRay;
		bool Visible;
	};

	float2 VirtualMotionReproject(float3 posW, float roughness, BRDF::ShadingData surface, float sampleRayT,
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
	
	Reservoir ReadReservoir(uint2 DTid, uint inputAIdx, uint inputBIdx)
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
		half M = asfloat16(uint16_t(resA.z >> 16));
		
		Reservoir r = Reservoir::Init(w_sum, W, M, wi, Li);

		return r;
	}

	Reservoir PartialReadReservoir_Reuse(uint2 DTid, uint inputAIdx)
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
		half M = asfloat16(uint16_t(resA.z >> 16));
		
		Reservoir r = Reservoir::Init(0, W, M, wi, Li);

		return r;
	}
	
	float PartialReadReservoir_WSum(uint2 DTid, uint inputBIdx)
	{
		Texture2D<float> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
		return g_reservoir_B[DTid];
	}
	
	float PartialReadReservoir_M(uint2 DTid, uint inputAIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];

		const uint resA_z = g_reservoir_A[DTid].z;
		float M = asfloat16(uint16_t(resA_z >> 16));
		return M;
	}

	// skips w_sum
	Reservoir PartialReadReservoir_Shading(uint2 DTid, uint inputAIdx)
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
		half M = asfloat16(uint16_t(resA.z >> 16));

		Reservoir r = Reservoir::Init(0, W, M, wi, Li);

		return r;
	}
	
	void WriteReservoir(uint2 DTid, Reservoir r, uint outputAIdx, uint outputBIdx, half m_max)
	{
		RWTexture2D<uint4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<float> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];

		uint16_t2 wi = asuint16(half2(r.wi.x, r.wi.z));
		uint16_t3 Li = asuint16(half3(r.Le));
		uint16_t M = asuint16(min(r.M, m_max));
		
		uint a_x = (uint(wi.y) << 16) | wi.x;
		uint a_y = (uint(Li.y) << 16) | Li.x;
		uint a_z = (uint(M) << 16) | Li.z;
		uint a_w = asuint(r.W);
		
		g_outReservoir_A[DTid] = uint4(a_x, a_y, a_z, a_w);
		g_outReservoir_B[DTid] = r.w_sum;
	}
}

#endif