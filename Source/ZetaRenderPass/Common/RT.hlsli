#ifndef RT_H
#define RT_H

#include "../../ZetaCore/RayTracing/RtCommon.h"
#include "../Common/Math.hlsli"

namespace RT
{
	// basis*: view-space basis vectors in world-space coordinates
	float3 GeneratePinholeCameraRay(uint2 pixel, float2 renderDim, float aspectRatio, float tanHalfFOV,
		float3 viewBasisX, float3 viewBasisY, float3 viewBasisZ, float2 jitter = 0)
	{
		float2 uv = (pixel + 0.5f + jitter) / renderDim;
		float2 ndc = Math::Transform::NDCFromUV(uv);
		ndc *= tanHalfFOV;
		ndc.x *= aspectRatio;

		float3 dirV = float3(ndc, 1);
		float3 dirW = dirV.x * viewBasisX + dirV.y * viewBasisY + dirV.z * viewBasisZ;

		return normalize(dirW);
	}

	// Ref: C. Wachter and N. Binder, "A Fast and Robust Method for Avoiding Self-Intersection", in Ray Tracing Gems 1, 2019.
	// Geometric Normal points outward for rays exiting the surface, else should be flipped.
	float3 OffsetRayRTG(float3 pos, float3 geometricNormal)
	{
		static const float origin = 1.0f / 32.0f;
		static const float float_scale = 1.0f / 65536.0f;
		static const float int_scale = 256.0f;

		//int3 of_i = int3(int_scale * geometricNormal.x, int_scale * geometricNormal.y, int_scale * geometricNormal.z);
		int3 of_i = int_scale * geometricNormal;

		float3 p_i = float3(
			asfloat(asint(pos.x) + ((pos.x < 0) ? -of_i.x : of_i.x)),
			asfloat(asint(pos.y) + ((pos.y < 0) ? -of_i.y : of_i.y)),
			asfloat(asint(pos.z) + ((pos.z < 0) ? -of_i.z : of_i.z)));

		float3 adjusted = float3(abs(pos.x) < origin ? pos.x + float_scale * geometricNormal.x : p_i.x,
			abs(pos.y) < origin ? pos.y + float_scale * geometricNormal.y : p_i.y,
			abs(pos.z) < origin ? pos.z + float_scale * geometricNormal.z : p_i.z);
		
		return adjusted;
	}

	float3 OffsetRay2(float3 origin, float3 dir, float3 normal, float minNormalBias = 5e-6f, float maxNormalBias = 1e-4)
	{
		const float maxBias = max(minNormalBias, maxNormalBias);
		const float normalBias = lerp(maxBias, minNormalBias, saturate(dot(normal, dir)));

		return origin + dir * normalBias;
	}

	// Ref: T. Akenine-Moller, J. Nilsson, M. Andersson, C. Barre-Brisebois, R. Toth 
	// and T. Karras, "Texture Level of Detail Strategies for Real-Time Ray Tracing," in 
	// Ray Tracing Gems 1, 2019.
	//
	// Usage (starting from the GBuffer):
	//		1. surfaceSpreadAngle = GetSurfaceSpreadAngleFromGBuffer()
	//		2. RayCone rc = Init()
	//		3. trace a ray to find the next vertex
	//		4. rc.Update(hitT, 0)
	//			4.1 lambda = rc.ComputeLambda(...)
	//			4.2 mipmapBias = rc.ComputeTextureMipmapOffset(lambda, ...)
	//			4.2 Do texture sampling using mipmapBias...
	//		5. goto 3	
	struct RayCone
	{
		static RayCone Init(float pixelSpreadAngle)
		{
			RayCone r;

			r.Width = 0;
			r.SpreadAngle = half(pixelSpreadAngle);
	
			return r;
		}

		static RayCone InitFromPrimaryHit(float pixelSpreadAngle, float surfaceSpreadAngle, float t)
		{
			RayCone r;

			r.Width = half(pixelSpreadAngle * t);
			r.SpreadAngle = half(pixelSpreadAngle + surfaceSpreadAngle);
	
			return r;
		}

		void Update(float t, float surfaceSpreadAngle)
		{
			this.Width = half(this.Width + t * this.SpreadAngle);
			this.SpreadAngle = half(this.SpreadAngle + surfaceSpreadAngle);
		}

		float Lambda(float3 v0, float3 v1, float3 v2, float2 t0, float2 t1, float2 t2, float ndotwo)
		{
			float P_a = length(cross((v1 - v0), (v2 - v0)));
			float T_a = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

			float lambda = T_a * this.Width * this.Width;
			lambda /= (P_a * ndotwo * ndotwo);

			return lambda;
		}

		static float TextureMipmapOffset(float lambda, float w, float h)
		{
			float mip = lambda * w * h;
			return 0.5f * log2(mip);
		}

		half Width;
		half SpreadAngle;
	};
}

#endif