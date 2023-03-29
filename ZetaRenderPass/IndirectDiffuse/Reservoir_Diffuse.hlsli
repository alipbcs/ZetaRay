#ifndef RESERVOIR_H
#define RESERVOIR_H

#include "ReSTIR_GI_Diffuse_Common.h"
#include "../Common/Sampling.hlsli"

#define INVALID_SAMPLE_POS 32768.xxx
#define INVALID_SAMPLE_NORMAL 32768.xx
#define INVALID_RAY_T -1
#define INCLUDE_COSINE_TERM_IN_TARGET 0
#define MAX_TEMPORAL_M 10
#define MAX_SPATIAL_M 100

struct DiffuseSample
{
	float3 Pos;
	float3 Lo;
	half2 Normal;
	half RayT;
};

struct DiffuseReservoir
{
	static DiffuseReservoir Init()
	{
		DiffuseReservoir res;
		
		res.M = 0;
		res.w_sum = 0.0f;
		res.SamplePos = INVALID_SAMPLE_POS;
		res.SampleNormal = INVALID_SAMPLE_NORMAL;
		res.Li = 0.0.xxx;
		
		return res;
	}
	
	static DiffuseReservoir Init(float3 samplePos, float w_sum, half3 Li, half M, half2 sampleNormal)
	{
		DiffuseReservoir res;
		
		res.M = M;
		res.w_sum = w_sum;
		res.SamplePos = samplePos;
		res.SampleNormal = sampleNormal;
		res.Li = Li;
		
		return res;		
	}
	
	DiffuseSample GetSample()
	{
		DiffuseSample s;
		s.Pos = this.SamplePos;
		s.Normal = this.SampleNormal;
		s.Lo = this.Li;
		return s;
	}
	
	float GetW()
	{
#if INCLUDE_COSINE_TERM_IN_TARGET
		return this.w_sum / max(1e-6, this.M * Math::Color::LuminanceFromLinearRGB(this.Li) * this.CosTheta);
#else
		return this.w_sum / max(1e-6, this.M * Math::Color::LuminanceFromLinearRGB(this.Li));
#endif		
	}
	
	void Update(float w, DiffuseSample s, inout RNG rng)
	{	
		this.w_sum += w;
		this.M += 1;
		
		if (rng.RandUniform() < (w / max(1e-6f, this.w_sum)))
		{
			this.SamplePos = s.Pos;
			this.SampleNormal = s.Normal;
			this.Li = half3(s.Lo);
		}
	}
	
	void Combine(DiffuseReservoir r, float3 wi, float3 normal, float maxM, float weight, float jacobianDet, inout RNG rng)
	{
		float clampedM = min(r.M, maxM);
		float weightedM = clampedM * weight;
		
		// using neighbor's Li is not necessarily correct if sample point's BRDF is 
		// view-dependant, as radiance reflected back in the resued path wouldn't necessarily
		// be the same as radiance received by the current pixel's surface point
		
#if INCLUDE_COSINE_TERM_IN_TARGET
		float cosTheta = saturate(dot(wi, normal));
		float p_q = Math::Color::LuminanceFromLinearRGB(r.Li) * cosTheta;
#else
		float p_q = Math::Color::LuminanceFromLinearRGB(r.Li);
#endif
		float p_q_corrected = p_q / jacobianDet;
		float w = p_q_corrected * r.GetW() * weightedM;
		float prevM = this.M;
		
		Update(w, r.GetSample(), rng);

		this.M = half(prevM + weightedM);
	}
	
	float3 SamplePos;
	float w_sum;
	half2 SampleNormal;
	half3 Li;
	half M;
};

namespace RGI_Diff_Util
{
	float JacobianDeterminant(float3 x1_q, float3 x2_q, float3 wi, float3 secondToFirst_r, DiffuseReservoir neighborReservoir)
	{
		const float3 secondToFirst_q = x1_q - x2_q;

		const float3 normalAtSecondVertex = Math::Encoding::DecodeUnitNormal(neighborReservoir.SampleNormal);
		const float cosPhi2_r = saturate(abs(dot(-wi, normalAtSecondVertex)));
		const float cosPhi2_q = saturate(abs(dot(normalize(secondToFirst_q), normalAtSecondVertex)));

		float jacobianDet = dot(secondToFirst_q, secondToFirst_q) / max(dot(secondToFirst_r, secondToFirst_r), 1e-6);
		//jacobianDet *= abs(cosPhi2_r - cosPhi2_q) < 1e-6 ? 1.0 : cosPhi2_r / max(cosPhi2_q, 1e-6);
		jacobianDet *= cosPhi2_r / max(cosPhi2_q, 1e-6);
		jacobianDet = max(jacobianDet, 0.95f); // w_sum blows up otherwise
	
		return jacobianDet;
	}

	float3 PackSample(half2 normal, float3 Li, half rayT)
	{
		uint3 r;
		half3 Lih = half3(Li);
	
		r.x = asuint16(Lih.r) | (uint(asuint16(Lih.g)) << 16);
		r.y = asuint16(Lih.b) | (uint(asuint16(rayT)) << 16);
		r.z = asuint16(normal.x) | (uint(asuint16(normal.y)) << 16);
	
		return asfloat(r);
	}

	void UnpackSample(float3 packed, out half2 normal, out float3 Li, out half rayT)
	{
		uint3 packedU = asuint(packed);
	
		uint16_t3 packedLi = uint16_t3(packedU.x & 0xffff, packedU.x >> 16, packedU.y & 0xffff);
		Li = float3(asfloat16(packedLi));
	
		rayT = asfloat16(uint16_t(packedU.y >> 16));

		uint16_t2 packedNormal = uint16_t2(packedU.z & 0xffff, packedU.z >> 16);
		normal = asfloat16(packedNormal);
	}

	DiffuseReservoir ReadInputReservoir(uint2 DTid, uint inputAIdx, uint inputBIdx, uint inputCIdx)
	{
		Texture2D<float4> g_inReservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_inReservoir_B = ResourceDescriptorHeap[inputBIdx];
		Texture2D<half2> g_inReservoir_C = ResourceDescriptorHeap[inputCIdx];

		const float4 resA = g_inReservoir_A[DTid];
		const half4 resB = g_inReservoir_B[DTid];
		const half2 resC = g_inReservoir_C[DTid];
	
		DiffuseReservoir r = DiffuseReservoir::Init(resA.xyz, resA.w, resB.xyz, resB.w, resC);

		return r;
	}

	DiffuseReservoir ReadInputReservoir(SamplerState s, float2 uv, uint inputAIdx, uint inputBIdx, uint inputCIdx)
	{
		Texture2D<float4> g_inReservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_inReservoir_B = ResourceDescriptorHeap[inputBIdx];
		Texture2D<half2> g_inReservoir_C = ResourceDescriptorHeap[inputCIdx];

		const float4 resA = g_inReservoir_A.SampleLevel(s, uv, 0.0);
		const half4 resB = g_inReservoir_B.SampleLevel(s, uv, 0.0);
		const half2 resC = g_inReservoir_C.SampleLevel(s, uv, 0.0);
	
		DiffuseReservoir r = DiffuseReservoir::Init(resA.xyz, resA.w, resB.xyz, resB.w, resC);

		return r;
	}

	// skips writing the reservoir's normal if it's not needed anymore to save bandwidth
	DiffuseReservoir PartialReadInputReservoir(uint2 DTid, uint inputAIdx, uint inputBIdx)
	{
		Texture2D<float4> g_inReservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_inReservoir_B = ResourceDescriptorHeap[inputBIdx];

		const float4 resA = g_inReservoir_A[DTid];
		const half4 resB = g_inReservoir_B[DTid];
	
		DiffuseReservoir r = DiffuseReservoir::Init(resA.xyz, resA.w, resB.xyz, resB.w, 0.0.xx);

		return r;
	}

	DiffuseReservoir PartialReadInputReservoir(SamplerState s, float2 uv, uint inputAIdx, uint inputBIdx)
	{
		Texture2D<float4> g_inReservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_inReservoir_B = ResourceDescriptorHeap[inputBIdx];

		//const float4 resA = g_inReservoir_A[DTid];
		const float4 resA = g_inReservoir_A.SampleLevel(s, uv, 0.0);
		const half4 resB = g_inReservoir_B.SampleLevel(s, uv, 0.0);
	
		DiffuseReservoir r = DiffuseReservoir::Init(resA.xyz, resA.w, resB.xyz, resB.w, 0.0.xx);

		return r;
	}

	void WriteOutputReservoir(uint2 DTid, DiffuseReservoir r, uint outputAIdx, uint outputBIdx, uint outputCIdx)
	{
		RWTexture2D<float4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<half4> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];
		RWTexture2D<half2> g_outReservoir_C = ResourceDescriptorHeap[outputCIdx];
	
		g_outReservoir_A[DTid] = float4(r.SamplePos, r.w_sum);
		g_outReservoir_B[DTid] = half4(r.Li, r.M);
		g_outReservoir_C[DTid] = r.SampleNormal;
	}

	// skips writing the reservoir's normal if it's not needed anymore to save bandwidth
	void PartialWriteOutputReservoir(uint2 DTid, DiffuseReservoir r, uint outputAIdx, uint outputBIdx)
	{
		RWTexture2D<float4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<half4> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];
	
		g_outReservoir_A[DTid] = float4(r.SamplePos, r.w_sum);
		g_outReservoir_B[DTid] = half4(r.Li, r.M);
	}
}
#endif