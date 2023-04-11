#ifndef RESERVOIR_SPECULAR_H
#define RESERVOIR_SPECULAR_H

#include "ReSTIR_GI_Specular_Common.h"
#include "../Common/Sampling.hlsli"
#include "../Common/BRDF.hlsli"

#define INVALID_SAMPLE_POS 32768.xxx
#define INVALID_SAMPLE_NORMAL 32768.xx

struct SpecularSample
{
	float3 Pos;
	float3 Lo;
	half2 Normal;
};

struct SpecularReservoir
{
	static SpecularReservoir Init()
	{
		SpecularReservoir res;
		
		res.SamplePos = INVALID_SAMPLE_POS;
		res.SampleNormal = INVALID_SAMPLE_NORMAL;
		res.Li = 0.0.xxx;
		res.M = 0;
		res.w_sum = 0.0f;
		res.BrdfCosTheta = 0.0.xxx;
		res.W = 0.0;
		
		return res;
	}
	
	static SpecularReservoir Init(float3 samplePos, float w_sum, float3 Li, float M, half2 sampleNormal, float3 brdfCosTheta, float W)
	{
		SpecularReservoir res;
		
		res.Li = Li;
		res.SamplePos = samplePos;
		res.SampleNormal = sampleNormal;
		res.M = M;
		res.w_sum = w_sum;
		res.BrdfCosTheta = brdfCosTheta;
		res.W = W;
		
		return res;
	}
		
	SpecularSample GetSample()
	{
		SpecularSample s;
		s.Pos = this.SamplePos;
		s.Normal = this.SampleNormal;
		s.Lo = this.Li;
		return s;
	}
	
	float ComputeW()
	{
		return this.w_sum / max(1e-6, this.M * Math::Color::LuminanceFromLinearRGB(this.Li * this.BrdfCosTheta));
	}
	
	float3 EvaluateRISEstimate()
	{
		float W = this.w_sum / max(1e-6, this.M * Math::Color::LuminanceFromLinearRGB(this.Li * this.BrdfCosTheta));
		return this.Li * this.BrdfCosTheta * W;
	}
	
	bool Update(float w, SpecularSample s, float3 brdfCosTheta, inout RNG rng)
	{
		this.w_sum += w;
		this.M += 1;
		
		if (rng.RandUniform() < (w / max(1e-6f, this.w_sum)))
		{
			this.SamplePos = s.Pos;
			this.SampleNormal = s.Normal;
			this.Li = s.Lo;
			this.BrdfCosTheta = brdfCosTheta;
			
			return true;
		}
		
		return false;
	}
	
	void Combine(SpecularReservoir r, float M_max, float weight, float jacobianDet, float3 brdfCostheta_r,
		inout RNG rng)
	{
		float clampedM = min(r.M, M_max);
		float weightedM = clampedM * weight;
		
		// using neighbor's Li is not necessarily correct if its sample point's BRDF is view dependent, as 
		// radiance that was reflected back in the resued path wouldn't necessarily be the same as radiance 
		// reflected towards the current pixel

		float p_q = Math::Color::LuminanceFromLinearRGB(r.Li * brdfCostheta_r);
		float p_q_corrected = p_q / jacobianDet;
		float neighborRISweight = p_q_corrected * r.W * weightedM;
		
		float prevM = this.M;
		
		Update(neighborRISweight, r.GetSample(), brdfCostheta_r, rng);

		this.M = prevM + weightedM;
	}
	
	// sample
	float3 SamplePos;
	half2 SampleNormal;
	float3 Li;
	
	// reservoir
	float w_sum;
	float M;
	float3 BrdfCosTheta;
	float W;
};

namespace RGI_Spec_Util
{
	float JacobianDeterminant(float3 x1_q, float3 x2_q, float3 wi, float3 secondToFirst_r, SpecularReservoir neighborReservoir)
	{
		const float3 secondToFirst_q = x1_q - x2_q;

		const float3 normalAtSecondVertex = Math::Encoding::DecodeUnitNormal(neighborReservoir.SampleNormal);
		const float cosPhi2_r = saturate(abs(dot(-wi, normalAtSecondVertex)));
		const float cosPhi2_q = saturate(abs(dot(normalize(secondToFirst_q), normalAtSecondVertex)));

		float jacobianDet = dot(secondToFirst_q, secondToFirst_q) / max(dot(secondToFirst_r, secondToFirst_r), 1e-6);
		//jacobianDet *= abs(cosPhi2_r - cosPhi2_q) < 1e-6 ? 1.0 : cosPhi2_r / max(cosPhi2_q, 1e-6);
		jacobianDet *= cosPhi2_r / max(cosPhi2_q, 1e-6);
		jacobianDet = max(jacobianDet, 0.91f); // w_sum blows up otherwise
	
		return jacobianDet;
	}
	
	float2 VirtualMotionReproject(float3 posW, float roughness, BRDF::SurfaceInteraction surface, float sampleRayT,
		float k, float linearDepth, float tanHalfFOV, float4x4 prevViewProj, inout float reflectionRayT)
	{
		// Ref: https://phys.libretexts.org/Bookshelves/University_Physics/Book%3A_University_Physics_(OpenStax)/University_Physics_III_-_Optics_and_Modern_Physics_(OpenStax)/02%3A_Geometric_Optics_and_Image_Formation/2.03%3A_Spherical_Mirrors
		// For a spherical mirror radius of curvature R = 1 / k where k denotes curvature. Then:
		//		1 / d_o + 1 / d_i = 2 / R
		//		1 / d_o + 1 / d_i = 2k
		// 
		// where d_i and d_o denote distance along the optical axis (surface normal) for the image and object 
		// respectively. Let w_o be the unit direction vector from the surface towards the eye, then ray 
		// distances t_i and t_o are derived as follows:
		//		t_o = d_o / ndotw_o
		//		t_i = -d_o / ndotw_o
		//
		// Replacing into above gives
		//		t_i = t_o / (1 - 2 k ndotw_o t_o)
		//
		// Note that as a convention, the mirror equation assumes convex and concave surfaces have negative and positive
		// radius of curvatures respectively.
	
		float pixelWidth = 2.0 * tanHalfFOV * linearDepth / max(surface.ndotwo, 1e-4);
	
		// curvature computation above uses the opposite signs
		k *= -1.0f;
		k *= pixelWidth;
		reflectionRayT = sampleRayT / (1.0f - 2 * k * sampleRayT * surface.whdotwo); // = 0 when suface is flat
	
		// interpolate between virtual motion and surface motion using GGX dominant factor
		// Ref: D. Zhdan, "ReBLUR: A Hierarchical Recurrent Denoiser," in Ray Tracing Gems 2, 2021.
		float factor = BRDF::SpecularBRDFGGXSmithDominantFactor(surface.whdotwo, roughness);
		float3 virtualPos = posW - surface.wo * reflectionRayT * factor;
	
		float4 virtualPosNDC = mul(prevViewProj, float4(virtualPos, 1.0f));
		float2 prevUV = Math::Transform::UVFromNDC(virtualPosNDC.xy / virtualPosNDC.w);

		return prevUV;
	}

	float3 RecalculateSpecularBRDF(float3 wi, float3 baseColor, float metallicFactor, inout BRDF::SurfaceInteraction surface)
	{
		float3 wh = normalize(wi + surface.wo);
		surface.ndotwh = saturate(dot(surface.shadingNormal, wh));
		surface.ndotwi = saturate(dot(surface.shadingNormal, wi));
			
		float3 F0 = lerp(0.04f.xxx, baseColor, metallicFactor);
		surface.whdotwo = saturate(dot(wh, surface.wo)); // = whdotwi
		surface.F = BRDF::FresnelSchlick(F0, surface.whdotwo);
		
		float3 brdfCostheta = BRDF::SpecularBRDFGGXSmith(surface);
		
		return brdfCostheta;
	}
	
	// skips sample position and normal
	SpecularReservoir PartialReadReservoir_Shading(uint2 DTid, uint inputAIdx, uint inputBIdx, uint inputDIdx)
	{
		Texture2D<float4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
		Texture2D<half4> g_reservoir_D = ResourceDescriptorHeap[inputDIdx];

		const float w_sum = g_reservoir_A[DTid].w;
		const float3 Li = g_reservoir_B[DTid].rgb;
		const float M = g_reservoir_B[DTid].w;
		const float3 brdfCostheta = g_reservoir_D[DTid].rgb;
		SpecularReservoir r = SpecularReservoir::Init(0.0.xxx, w_sum, Li, M, 0.0.xx, brdfCostheta, 0.0);

		return r;
	}

	// skips w_sum and BrdfCosTheta
	SpecularReservoir PartialReadReservoir_Reuse(uint2 DTid, uint inputAIdx, uint inputBIdx, uint inputCIdx, uint inputDIdx)
	{
		Texture2D<float4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
		Texture2D<half2> g_reservoir_C = ResourceDescriptorHeap[inputCIdx];
		Texture2D<half4> g_reservoir_D = ResourceDescriptorHeap[inputDIdx];

		const float3 pos = g_reservoir_A[DTid].xyz;
		const float3 Li = g_reservoir_B[DTid].rgb;
		const float M = g_reservoir_B[DTid].w;
		const half2 normal = g_reservoir_C[DTid];
		const float W = g_reservoir_D[DTid].a;
		SpecularReservoir r = SpecularReservoir::Init(pos, 0.0, Li, M, normal, 0.0.xxx, W);

		return r;
	}

	// skips W
	SpecularReservoir PartialReadReservoir_NoW(uint2 DTid, uint inputAIdx, uint inputBIdx, uint inputCIdx, uint inputDIdx)
	{
		Texture2D<float4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
		Texture2D<half4> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
		Texture2D<half2> g_reservoir_C = ResourceDescriptorHeap[inputCIdx];
		Texture2D<half4> g_reservoir_D = ResourceDescriptorHeap[inputDIdx];

		const float3 pos = g_reservoir_A[DTid].xyz;
		const float w_sum = g_reservoir_A[DTid].w;
		const float3 Li = g_reservoir_B[DTid].rgb;
		const float M = g_reservoir_B[DTid].w;
		const half2 normal = g_reservoir_C[DTid];
		const float3 brdfCostheta = g_reservoir_D[DTid].rgb;
		SpecularReservoir r = SpecularReservoir::Init(pos, w_sum, Li, M, normal, brdfCostheta, 0);

		return r;
	}
	
	// skips sample normal and W
	void PartialWriteReservoir_NoNormalW(uint2 DTid, SpecularReservoir r, uint outputAIdx, uint outputBIdx, uint outputDIdx)
	{
		RWTexture2D<float4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<half4> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];
		RWTexture2D<half4> g_outReservoir_D = ResourceDescriptorHeap[outputDIdx];
	
		g_outReservoir_A[DTid] = float4(r.SamplePos, r.w_sum);
		g_outReservoir_B[DTid] = half4(r.Li, r.M);
		g_outReservoir_D[DTid].rgb = half3(r.BrdfCosTheta);
	}
	
	void WriteReservoir(uint2 DTid, SpecularReservoir r, uint outputAIdx, uint outputBIdx, uint outputCIdx, uint outputDIdx)
	{
		RWTexture2D<float4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
		RWTexture2D<half4> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];
		RWTexture2D<half2> g_outReservoir_C = ResourceDescriptorHeap[outputCIdx];
		RWTexture2D<half4> g_outReservoir_D = ResourceDescriptorHeap[outputDIdx];
	
		// clamp W to about maximum value possible with 16-bit floats
		const float W = min(r.ComputeW(), 65472);
		
		g_outReservoir_A[DTid] = float4(r.SamplePos, r.w_sum);
		g_outReservoir_B[DTid] = half4(r.Li, r.M);
		g_outReservoir_C[DTid] = r.SampleNormal;
		g_outReservoir_D[DTid] = half4(r.BrdfCosTheta, W);
	}
}

#endif