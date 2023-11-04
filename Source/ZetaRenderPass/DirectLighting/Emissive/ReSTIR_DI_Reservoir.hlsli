#ifndef RESTIR_DI_RESERVOIR_H
#define RESTIR_DI_RESERVOIR_H

#include "DirectLighting_Common.h"
#include "../../Common/Sampling.hlsli"

#define KAHAN_SUMMATION 0

namespace RDI_Util
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
			res.LightIdx = -1;
			res.Bary = 0.0.xx;
#if KAHAN_SUMMATION == 1
			res.Compensation = 0.0f;
#endif
			return res;
		}

		static Reservoir Init(float w, half m, float3 le, uint lightIdx, float2 bary)
		{
			Reservoir res;
			res.w_sum = 0;
			res.Le = le;
			res.M = m;
			res.W = w;
			res.LightIdx = lightIdx;
			res.Bary = bary;
#if KAHAN_SUMMATION == 1
			res.Compensation = 0.0f;
#endif

			return res;
		}
		
		bool IsValid()
		{
			return this.LightIdx != -1;
		}

		bool Update(float w, float3 le, uint lightIdx, float2 bary, inout RNG rng)
		{
#if KAHAN_SUMMATION == 1
			float corrected = w - this.Compensation;
			float newSum = this.w_sum + corrected;
			this.Compensation = (newSum - this.w_sum) - corrected;
			this.w_sum = newSum;
#else
			this.w_sum += w;
#endif
			this.M += 1;

			if (rng.Uniform() < (w / max(1e-6f, this.w_sum)))
			{
				this.Le = le;
				this.LightIdx = lightIdx;
				this.Bary = bary;

				return true;
			}

			return false;
		}

		float w_sum;
#if KAHAN_SUMMATION == 1
		precise float Compensation;
#endif
		float W;
		float3 Le;
		uint LightIdx;
		float2 Bary;
		half M;
	};

	Reservoir PartialReadReservoir_Reuse(uint2 DTid, uint inputAIdx, uint inputBIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<uint2> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];

		const uint4 resA = g_reservoir_A[DTid];
		const float W = asfloat(resA.w);

		const float3 Le = asfloat16(uint16_t3(resA.y & 0xffff, resA.y >> 16, resA.z & 0xffff));
		const half M = asfloat16(uint16_t(resA.z >> 16));
		const float2 bary = asfloat16(uint16_t2(resA.x & 0xffff, resA.x >> 16));
		const uint lightIdx = g_reservoir_B[DTid].x;

		return Reservoir::Init(W, M, Le, lightIdx, bary);
	}

	uint PartialReadReservoir_ReuseLightIdx(uint2 DTid, uint inputBIdx)
	{
		Texture2D<uint2> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
		const uint lightIdx = g_reservoir_B[DTid].x;

		return lightIdx;
	}

	Reservoir PartialReadReservoir_ReuseRest(uint2 DTid, uint inputAIdx, uint lightIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];

		const uint4 resA = g_reservoir_A[DTid];
		float W = asfloat(resA.w);
		
		float3 Le = asfloat16(uint16_t3(resA.y & 0xffff, resA.y >> 16, resA.z & 0xffff));
		half M = asfloat16(uint16_t(resA.z >> 16));
		float2 bary = asfloat16(uint16_t2(resA.x & 0xffff, resA.x >> 16));

		return Reservoir::Init(W, M, Le, lightIdx, bary);
	}

	float PartialReadReservoir_WSum(uint2 DTid, uint inputBIdx)
	{
		Texture2D<uint2> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
		return asfloat(g_reservoir_B[DTid].y);
	}

	half PartialReadReservoir_M(uint2 DTid, uint inputAIdx)
	{
		Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		return asfloat16(uint16_t(g_reservoir_A[DTid].z >> 16));
	}

	void WriteReservoir(uint2 DTid, Reservoir r, uint outputAIdx, uint outputBIdx, half m_max)
	{
		RWTexture2D<uint4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<uint2> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];

		uint16_t3 Le = asuint16(half3(r.Le));
		uint16_t M = asuint16(min(r.M, m_max));
		uint16_t2 bary = asuint16(half2(r.Bary));

		uint a_x = (uint(bary.y) << 16) | bary.x;
		uint a_y = (uint(Le.y) << 16) | Le.x;
		uint a_z = (uint(M) << 16) | Le.z;
		uint a_w = asuint(r.W);

		g_outReservoir_A[DTid] = uint4(a_x, a_y, a_z, a_w);
		g_outReservoir_B[DTid] = uint2(r.LightIdx, asuint(r.w_sum));
	}
}

#endif