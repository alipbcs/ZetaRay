#ifndef RT_H
#define RT_H

#include "../Common/Common.hlsli"
#include "../Common/LightSourceData.h"
#include "RtCommon.h"

// posTS is texture space position ([0, 1] * [0, 1])
// basisX, basisY, basisZ are basis vectors for the view space
// projWindowZ is equal to 1 / tan(FOV / 2) == ProjectionMatrix[1][1]
float3 GeneratePinholeCameraRay2(float2 posTS, float aspectRatio, float tanHalfFOV, 
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
float3 GeneratePinholeCameraRay(float2 posTS, float aspectRatio, float projWindowZ, 
	float3 viewBasisX, float3 viewBasisY, float3 viewBasisZ)
{
	// pixel i's center is on [i] + 0.5
//	float2 xy = (dtid + 0.5f.xx) / screenDim;
//	float2 xy = (DispatchRaysIndex().xy + float2(0.5f, 0.5f)) / DispatchRaysDimensions().xy;

	// Texture space [0, 1] -> NDC space [-1, 1]
	float2 posNDC = posTS * 2.0 - 1.0f;
	
	// In Direct3D +y on screen goes down
	posNDC.y = -posNDC.y;
	
	// projection window is at distance d(==1 / tand(FOV/2)) from camera with height 2 and 
	// width 2 * aspectRatio. So for any point on projection window:
	// x is in [-aspectRatio, +aspectRatio]
	// y is in [-1, 1]
	// z = 1 / tand(FOV/2)
	posNDC.x *= aspectRatio;
	float3 posV = float3(posNDC, projWindowZ);
	
	// to go from view-space to world-space, basis vectors for view-space relative to world-space are needed 
	float3 posW = viewBasisX * posV.x + viewBasisY * posV.y + viewBasisZ * posV.z;
	
	return normalize(posW);
}

// projWindowZ is equal to 1 / tan(FOV / 2) == ProjectionMatrix[1][1]
float3 GeneratePinholeCameraRayViewSpace(float2 posTS, float aspectRatio, float projWindowZ)
{
	// Texture space [0, 1] -> NDC space [-1, 1]
	float2 posNDC = posTS * 2.0 - 1.0f;
	
	// In Direct3D +y on screen goes down
	posNDC.y = -posNDC.y;
	
	// projection window is at distance d(==1 / tand(FOV/2)) from camera with height 2 and 
	// width 2 * aspectRatio. So for any point on projection window:
	// x is in [-aspectRatio, +aspectRatio]
	// y is in [-1, 1]
	// z = 1 / tand(FOV/2)
	posNDC.x *= aspectRatio;
	float3 posV = float3(posNDC, projWindowZ);
	
	return normalize(posV);
}

float3 OffsetRayOrigin(in float3 pos, in float3 geometricNormal, in float3 wi)
{
	const float ndotr = saturate(dot(geometricNormal, wi));
	const float ndotr3 = ndotr * ndotr * ndotr;
	const float3 adjustedRayOrigin = pos + (1.0f - ndotr3) * geometricNormal + ndotr3 * wi;

	return adjustedRayOrigin;
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
//		2. Desc = Build()
//		3. trace a ray to find next vertex in path
//		4. Update(hitT, 0, Desc)
//			4.1 mipmapBias = ComputeTextureMipmapOffset(Desc, ...
//			4.2 Do texture sampling ...
//		5. goto 3	

// Width: width of the cone at given hitpoint. Defined recursively as:
//		w_i = w_(i-1) + y_(i-1) * t
//
// SpreadAngle: recursively defined as:
//		y_i + y_(i-1) + beta_(i-1)
struct RayCone
{
	half Width;
	half SpreadAngle;
};

// Base case for the recursive definition of a Ray Cone. Base case builds
// the Ray Cone at primary hit using the data from GBuffer.
RayCone InitRayCone(half pixelSpreadAngle, half surfaceSpreadAngle, half t)
{
	RayCone rayCone;
	
	rayCone.Width = pixelSpreadAngle * t;
	rayCone.SpreadAngle = pixelSpreadAngle + surfaceSpreadAngle;
	
	return rayCone;
}

void UpdateRayCone(in half t, in half surfaceSpreadAngle, inout RayCone rayCone)
{
	// Width is used for current mipmap calculations
	rayCone.Width += t * rayCone.SpreadAngle;
	// spread angle for next vertex (not used for current mipmap calculations)
	rayCone.SpreadAngle += surfaceSpreadAngle;
}

// This version is more compact
float ComputeTextureMipmapOffsetRayCone(RayCone rayCone,
		float3 v0, float3 v1, float3 v2,
		float2 t0, float2 t1, float2 t2,
		float w, float h, float ndotwo)
{
	float Pa = length(cross((v1 - v0), (v2 - v0)));
	float Ta = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

	float wh = w * h;
	float lambda = Ta * wh * wh * rayCone.Width * rayCone.Width;
	lambda /= (Pa * ndotwo * ndotwo);
		
	return 0.5f * log2(lambda);
}

float ComputeLambdaRayCone(RayCone rayCone,
		float3 v0, float3 v1, float3 v2,
		float2 t0, float2 t1, float2 t2,
		float ndotwo)
{
	float Pa = length(cross((v1 - v0), (v2 - v0)));
	float Ta = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

	float lambda = Ta * rayCone.Width * rayCone.Width;
	lambda /= (Pa * ndotwo * ndotwo);
		
	return lambda;
}

float ComputeTextureMipmapOffsetRayCone(float lambda, float w, float h)
{
	float wh = w * h;
	lambda *= wh * wh;
	return 0.5f * log2(lambda);
}

namespace RayCones
{
	struct Desc
	{
		half Width;
		half SpreadAngle;
	};

	Desc Build(half pixelSpreadAngle, half surfaceSpreadAngle, half t)
	{
		Desc rayCone;
	
		rayCone.Width = pixelSpreadAngle * t;
		rayCone.SpreadAngle = pixelSpreadAngle + surfaceSpreadAngle;
	
		return rayCone;
	}
		
	void Update(in half t, in half surfaceSpreadAngle, inout Desc rayCone)
	{
		// Width is used for current mipmap calculations
		rayCone.Width += t * rayCone.SpreadAngle;
		// spread angle for next vertex (not used for current mipmap calculations)
		rayCone.SpreadAngle += surfaceSpreadAngle;
	}
	
	/* For reference
	// v0, v1, v2 are in world space
	// w, h are texture dims
	float ComputeTriangleMipmapOffsetConstant(float3 v0, float3 v1, float3 v2, float2 t0, float2 t1, float2 t2, float w, float h)
	{
		float Pa = length(cross((v1 - v0), (v2 - v0)));
		// (2D) cross-product gives the area
		float Ta = w * h * abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

		return 0.5f * log2(Ta / Pa);
	}

	float ComputeTextureMipmapOffset(
		float triangleMipmapOffsetConstant, 
		float w, float h,
		Desc rayCone, float3 normal, float3 wi)
	{
		float ndotwi = dot(normal, wi);
		
		float lambda = triangleMipmapOffsetConstant;
		lambda += 0.5f * log2((w * h * rayCone.Width * rayCone.Width) / (ndotwi * ndotwi));
		
		return lambda;
	}
	*/
	
	// This version is more compact
	float ComputeTextureMipmapOffset(
		Desc rayCone,
		float3 v0, float3 v1, float3 v2, 
		float2 t0, float2 t1, float2 t2,
		float w, float h, float ndotwo)
	{
		float Pa = length(cross((v1 - v0), (v2 - v0)));
		float Ta = abs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y));

		float wh = w * h;
		float lambda = Ta * wh * wh * rayCone.Width * rayCone.Width;
		lambda /= (Pa * ndotwo * ndotwo);
		
		return 0.5f * log2(lambda);
	}
}

Vertex InterpolateTriangleAttribs(Vertex v0, Vertex v1, Vertex v2, float2 bary)
{
	Vertex v;
	
	v.PosL = v0.PosL + bary.x * (v1.PosL - v0.PosL) + bary.y * (v2.PosL - v0.PosL);
	v.NormalL = v0.NormalL + bary.x * (v1.NormalL - v0.NormalL) + bary.y * (v2.NormalL - v0.NormalL);
	v.TexUV = v0.TexUV + bary.x * (v1.TexUV - v0.TexUV) + bary.y * (v2.TexUV - v0.TexUV);
	v.TangentU = v0.TangentU + bary.x * (v1.TangentU - v0.TangentU) + bary.y * (v2.TangentU - v0.TangentU);
	
	return v;
}

#endif