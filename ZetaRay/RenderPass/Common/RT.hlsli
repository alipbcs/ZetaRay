#ifndef RT_H
#define RT_H

#include "RtCommon.h"

namespace RT
{
	// posTS is the texture space position ([0, 1] * [0, 1])
	// basisX, basisY, basisZ are view-space basis vectors 
	// projWindowZ is equal to 1 / tan(FOV / 2) == ProjectionMatrix[1][1]
	float3 GeneratePinholeCameraRayWorldSpace(float2 posTS, float aspectRatio, float tanHalfFOV,
		float3 viewBasisX, float3 viewBasisY, float3 viewBasisZ)
	{
		float2 posNDC = posTS * 2.0f - 1.0f;
		posNDC.y *= -1.0f;
		posNDC.x *= aspectRatio;
	
		float3 posV = float3(posNDC.x * tanHalfFOV, posNDC.y * tanHalfFOV, 1.0f);
		float3 dirW = posV.x * viewBasisX + posV.y * viewBasisY + posV.z * viewBasisZ;
	
		return normalize(dirW);
	}

	// projWindowZ is equal to 1 / tan(FOV / 2) == ProjectionMatrix[1][1]
	float3 GeneratePinholeCameraRayViewSpace(float2 posTS, float aspectRatio, float projWindowZ)
	{
		// Texture space [0, 1] -> NDC space [-1, 1]
		float2 posNDC = posTS * 2.0 - 1.0f;
	
		// in Direct3D +y on screen goes down
		posNDC.y = -posNDC.y;
	
		// projection window is at distance d(==1 / tand(FOV/2)) from camera with height 2 and 
		// width 2 * aspectRatio. So for any point on the projection window:
		// x is in [-aspectRatio, +aspectRatio]
		// y is in [-1, 1]
		// z = 1 / tand(FOV/2)
		posNDC.x *= aspectRatio;
		float3 posV = float3(posNDC, projWindowZ);
	
		return normalize(posV);
	}

	// Ref: "Ray Tracing Gems 1, Chapter 6"
	// Geometric Normal points outward for rays exiting the surface, else should be flipped.
	float3 OffsetRayRTG(in float3 p, in float3 geometricNormal)
	{
		const float origin = 1.0f / 32.0f;
		const float float_scale = 1.0f / 65536.0f;
		const float int_scale = 256.0f;
	
		int3 of_i = int3(int_scale * geometricNormal.x,
					 int_scale * geometricNormal.y,
					 int_scale * geometricNormal.z);

		float3 p_i = float3(asfloat(asint(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
						asfloat(asint(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
						asfloat(asint(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

		return float3(abs(p.x) < origin ? p.x + float_scale * geometricNormal.x : p_i.x,
				  abs(p.y) < origin ? p.y + float_scale * geometricNormal.y : p_i.y,
				  abs(p.z) < origin ? p.z + float_scale * geometricNormal.z : p_i.z);
	}

	// Ref: "Ray Tracing Gems - Chapter 20"
	// Usage (Starting from GBuffer (primary hit)):
	//		1. surfaceSpreadAngle = GetSurfaceSpreadAngleFromGBuffer()
	//		2. RayCone rc = Init()
	//		3. trace a ray to find the next vertex in path
	//		4. rc.Update(hitT, 0)
	//			4.1 lambda = rc.ComputeLambda(...)
	//			4.2 mipmapBias = rc.ComputeTextureMipmapOffset(lambda, ...)
	//			4.2 Do texture sampling using mipmapBias...
	//		5. goto 3	
	struct RayCone
	{
		static RayCone Init(float pixelSpreadAngle, float surfaceSpreadAngle, float t)
		{
			RayCone r;
	
			r.Width = half(pixelSpreadAngle * t);
			r.SpreadAngle = half(pixelSpreadAngle + surfaceSpreadAngle);
	
			return r;
		}
		
		void Update(float t, float surfaceSpreadAngle)
		{
			// Width is used for current mipmap calculations
			this.Width += half(t * this.SpreadAngle);
			// spread angle for next vertex (not used for current mipmap calculations)
			this.SpreadAngle += half(surfaceSpreadAngle);
		}
	
		float ComputeLambda(float3 v0, float3 v1, float3 v2,
			float2 t0, float2 t1, float2 t2, float ndotwo)
		{
			float Pa = length(cross((v1 - v0), (v2 - v0)));
			float Ta = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

			float lambda = Ta * this.Width * this.Width;
			lambda /= (Pa * ndotwo * ndotwo);
		
			return lambda;
		}

		static float ComputeTextureMipmapOffset(float lambda, float w, float h)
		{
			float wh = w * h;
			lambda *= wh * wh;
			
			return 0.5f * log2(lambda);
		}
		
		half Width;
		half SpreadAngle;
	};
}

#endif