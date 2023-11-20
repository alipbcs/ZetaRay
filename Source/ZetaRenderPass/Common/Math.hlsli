#ifndef MATH_H
#define MATH_H

#define PI					3.141592654f
#define TWO_PI				6.283185307f
#define PI_OVER_2			1.570796327f
#define PI_OVER_4			0.7853981635f
#define THREE_PI_OVER_2		4.7123889804f
#define ONE_OVER_PI			0.318309886f
#define ONE_OVER_2_PI		0.159154943f
#define ONE_OVER_4_PI		0.079577472f
#define TWO_OVER_PI			0.636619772f
#define FLT_MIN				1.175494351e-38 
#define FLT_MAX				3.402823466e+38 

namespace Math
{
	// Returns whether pos is in [0, dim).
	template<typename T>
	bool IsWithinBounds(T pos, T dim)
	{
		return all(pos >= 0) && all(pos < dim);
	}

	// Returns smallest floating point f' such that f' > f.
	float NextFloat32Up(float f)
	{
		uint u = asuint(f);
		u = f >= 0 ? u + 1 : u - 1;
	
		return asfloat(u);
	}
	
	uint RoundUpToPowerOf2(uint x)
	{
		x--;
		x |= x >> 1;
		x |= x >> 2;
		x |= x >> 4;
		x |= x >> 8;
		x |= x >> 16;
		x++;
	
		return x;
	}
	
	float3x3 Inverse(float3x3 M)
	{
		// Given 3x3 matrix M = [u, v, w] where u,v,w are column vectors, M^(-1) is given by
		//		M^(-1) = [a b c]^T
		//
		// where 
		//		a = (v * w) / u.(v * w)
		//		b = (w * u) / u.(v * w)
		//		c = (u * v) / u.(v * w)		
		const float3 u = float3(M._11, M._21, M._31);
		const float3 v = float3(M._12, M._22, M._32);
		const float3 w = float3(M._13, M._23, M._33);

		const float3 vCrossW = cross(v, w);
		const float det = dot(u, vCrossW);
		
		const float3 a = vCrossW / det;
		const float3 b = cross(w, u) / det;
		const float3 c = cross(u, v) / det;

		return float3x3(a, b, c);
	}
	
	// Input in [-1, 1] and output in [0, PI]
	// Ref: https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
	template<typename T>
	T ArcCos(T x)
	{
		T xAbs = abs(x);
		T res = ((-0.0206453f * xAbs + 0.0764532f) * xAbs + -0.21271f) * xAbs + 1.57075f;
		res *= sqrt(1.0f - xAbs);

		return select((x >= 0), res, PI - res);
	}

	float3 SphericalToCartesian(float r, float theta, float phi)
	{
		float sinTheta = sin(theta);
		return float3(r * sinTheta * cos(phi), r * cos(theta), -r * sinTheta * sin(phi));
	}

	float2 SphericalFromCartesian(float3 w)
	{
		float2 thetaPhi;
		
		// x = sin(theta) * cos(phi)
		// y = cos(theta)
		// z = -sin(theta) * sin(phi)
		thetaPhi.x = ArcCos(w.y);
		// phi is measured clockwise from the x-axis and atan2 uses the sign to figure out the quadrant
		thetaPhi.y = atan2(-w.z, w.x);									// [-PI, +PI]
		thetaPhi.y = thetaPhi.y < 0 ? thetaPhi.y + TWO_PI : thetaPhi.y; // [0, 2 * PI]
		
		return thetaPhi;
	}
	
	float TriangleArea(float3 v0, float3 v1, float3 v2)
	{
		return 0.5f * length(cross(v1 - v0, v2 - v0));
	}
	
	template<typename T>
	T SignNotZero(T v) 
	{
		return select(v >= 0, +1.0f, -1.0f);
	}

	template<typename T>
	T LinearDepthFromNDC(T z_NDC, float near)
	{
		return select(z_NDC == 0.0f, FLT_MAX, near / z_NDC);
	}
	
	float2 NDCFromUV(float2 uv)
	{
		float2 ndc = uv * 2.0f - 1.0f;
		ndc.y = -ndc.y;

		return ndc;
	}

	float2 UVFromNDC(float2 ndc)
	{
		return ndc * float2(0.5, -0.5) + 0.5f;
	}

	float2 ScreenSpaceFromNDC(float2 ndc, float2 screenDim)
	{
		// [-1, 1] * [-1, 1] -> [0, 1] * [0, 1]
		float2 posSS = ndc * float2(0.5f, -0.5f) + 0.5f;
		posSS *= screenDim;

		return posSS;
	}

	float2 UVFromScreenSpace(uint2 posSS, float2 screenDim)
	{
		float2 uv = float2(posSS) + 0.5f;
		uv /= screenDim;

		return uv;
	}

	float3 WorldPosFromUV(float2 uv, float2 screenDim, float z_view, float tanHalfFOV, float aspectRatio, 
		float3x4 viewInv, float2 jitter = 0.0)
	{
		float2 posV = NDCFromUV(uv) + jitter / screenDim;
		posV *= tanHalfFOV;
		posV.x *= aspectRatio;
		float3 posW = float3(posV, 1.0f) * z_view;
		posW = mul(viewInv, float4(posW, 1.0f));

		return posW;
	}

	float3 WorldPosFromScreenSpace(float2 posSS, float2 screenDim, float z_view, float tanHalfFOV,
		float aspectRatio, float3x4 viewInv, float2 jitter = 0.0)
	{
		float2 uv = (posSS + 0.5f + jitter) / screenDim;
		float2 posV = NDCFromUV(uv);
		posV *= tanHalfFOV;
		posV.x *= aspectRatio;
		float3 posW = float3(posV, 1.0f) * z_view;
		posW = mul(viewInv, float4(posW, 1.0f));

		return posW;
	}

	float2 UVFromWorldPos(float3 posW, float2 screenDim, float tanHalfFOV, float aspectRatio, 
		float3x4 view, float2 jitter = 0.0)
	{
		float3 posV = mul(view, float4(posW, 1.0f));
		float2 ndc = posV.xy / (tanHalfFOV * posV.z);
		ndc.x /= aspectRatio;

		float2 uv = Math::UVFromNDC(ndc);
		uv += jitter / screenDim;

		return uv;
	}
	
	float3 TangentSpaceToWorldSpace(float2 bumpNormal2, float3 tangentW, float3 normalW, float scale)
	{
		float3 bumpNormal = float3(2.0f * bumpNormal2 - 1.0f, 0.0f);
		bumpNormal.z = sqrt(1.0f - saturate(dot(bumpNormal, bumpNormal)));
		float3 scaledBumpNormal = bumpNormal * float3(scale, scale, 1.0f);
		
		// Invalid scale or bump, normalize() leads to NaN
		if (dot(scaledBumpNormal, scaledBumpNormal) < 1e-6f)
			return normalW;
		
		scaledBumpNormal = normalize(scaledBumpNormal);

		// Graham-Schmidt orthogonalization
		normalW = normalize(normalW);
		tangentW = normalize(tangentW - dot(tangentW, normalW) * normalW);

		// Change-of-coordinate transformation from TBN to world space
		float3 bitangentW = cross(normalW, tangentW);
		float3x3 TangentSpaceToWorld = float3x3(tangentW, bitangentW, normalW);

		return mul(scaledBumpNormal, TangentSpaceToWorld);
	}
	
	// Builds an orthonormal TBN coordinate system.
	// Ref: T. Duff, J. Burgess, P. Christensen, C. Hery, A. Kensler, M. Liani, 
	// R. Villemin, "Building an Orthonormal Basis, Revisited," Journal of Computer Graphics Techniques, 2017.
	void revisedONB(float3 n, out float3 b1, out float3 b2)
	{
		const float s = n.z >= 0.0f ? 1.0f : -1.0f;
		const float a = -1.0 / (s + n.z);
		const float b = n.x * n.y * a;
		b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
		b2 = float3(b, s + n.y * n.y * a, -n.y);
	}

	// Quaternion that rotates +Y to u. Assumes ||u|| = 1.
	float4 QuaternionFromY(float3 u)
	{
		float4 q;
		float real = 1.0f + u.y;

		// u = (0, -1, 0) is a singularity
		if (real > 1e-6)
		{
			// build rotation quaternion that maps y = (0, 1, 0) to u
			float3 yCrossU = float3(u.z, 0.0f, -u.x);
			q = float4(yCrossU, real);
			q = normalize(q);
		}
		else
		{
			// rotate 180 degrees around the X-axis (Z-axis works too since both are orthogonal to Y)
			//
			// rotation quaternion = (n_x * s, n_y * s, n_z * s, c)
			// where
			//		n = (1, 0, 0)
			//		s = sin(theta/2) = sin(90) = 1 and c = cos(theta/2) = cos(90) = 0
			q = float4(1.0f, 0.0f, 0.0f, 0.0f);
	
			// no need to normalize q
		}

		return q;
	}
	
	// Quaternion that rotates u to +Y. Assumes ||u|| = 1.
	float4 QuaternionToY(float3 u)
	{
		float4 q;
		float real = 1.0f + u.z;

		// u = (0, 0, -1) is a singularity
		if (real > 1e-6)
		{
			// build rotation quaternion that maps u to z = (0, 0, 1)
			float3 yCrossU = float3(-u.z, 0.0f, u.x);
			q = float4(yCrossU, real);
			q = normalize(q);
		}
		else
		{
			// rotate 180 degrees around the X-axis (Y-axis works too since both are orthogonal to Z)
			//
			// rotation quaternion = (n_x * s, n_y * s, n_z * s, c)
			// where
			//		n = (1, 0, 0)
			//		s = sin(theta/2) = sin(90) = 1 and c = cos(theta/2) = cos(90) = 0
			q = float4(1.0f, 0.0f, 0.0f, 0.0f);
	
			// no need to normalize q
		}

		return q;
	}
	
	// Quaternion that rotates +Z to u. Assumes ||u|| = 1.
	float4 QuaternionFromZ(float3 u)
	{
		float4 q;
		float real = 1.0f + u.z;

		// u = (0, 0, -1) is a singularity
		if (real > 1e-6)
		{
			// build rotation quaternion that maps z = (0, 0, 1) to u
			float3 zCrossU = float3(-u.y, u.x, 0);
			q = float4(zCrossU, real);
			q = normalize(q);
		}
		else
		{
			// rotate 180 degrees around the X-axis (Y-axis works too since both are orthogonal to Z)
			//
			// rotation quaternion = (n_x * s, n_y * s, n_z * s, c)
			// where
			//		n = (1, 0, 0)
			//		s = sin(theta/2) = sin(90) = 1 and c = cos(theta/2) = cos(90) = 0
			q = float4(1.0f, 0.0f, 0.0f, 0.0f);
	
			// no need to normalize q
		}

		return q;
	}
	
	// Quaternion that rotates u to +Z. Assumes ||u|| = 1.
	float4 QuaternionToZ(float3 u)
	{
		float4 q;
		float real = 1.0f + u.z;

		// u = (0, 0, -1) is a singularity
		if (real > 1e-6)
		{
			// build rotation quaternion that maps u to z = (0, 0, 1)
			float3 uCrossZ = float3(u.y, -u.x, 0);
			q = float4(uCrossZ, real);
			q = normalize(q);
		}
		else
		{
			// rotate 180 degrees around the X-axis (Y-axis works too since both are orthogonal to Z)
			//
			// rotation quaternion = (n_x * s, n_y * s, n_z * s, c)
			// where
			//		n = (1, 0, 0)
			//		s = sin(theta/2) = sin(90) = 1 and c = cos(theta/2) = cos(90) = 0
			q = float4(1.0f, 0.0f, 0.0f, 0.0f);
	
			// no need to normalize q
		}

		return q;
	}

	// Rotates v using unit quaternion q by computing q * u * q* where
	//		* denotes quaternion multiplication
	//		q* is the conjugate of q.
	// Ref: https://fgiesen.wordpress.com/2019/02/09/rotating-a-single-vector-using-a-quaternion/
	float3 RotateVector(float3 v, float4 q)
	{
		float3 imaginary = q.xyz;
		float real = q.w;

		float3 t = cross(2 * imaginary, v);
		float3 rotated = v + real * t + cross(imaginary, t);
		
		return rotated;
	}

	uint16_t2 EncodeAsUNorm2(float2 u)
	{
		return uint16_t2(round(clamp(u, 0.0f, 1.0f) * float((1 << 16) - 1)));
	}

	float2 DecodeUNorm2(uint16_t2 i)
	{
		return i / float((1 << 16) - 1);
	}

	int16_t2 EncodeAsSNorm2(float2 u)
	{
		u = clamp(u, -1, 1);
		return int16_t2(round(u * float((1 << 15) - 1)));
	}

	int16_t3 EncodeAsSNorm3(float3 u)
	{
		u = clamp(u, -1, 1);
		return int16_t3(round(u * float((1 << 15) - 1)));
	}

	float2 DecodeSNorm2(int16_t2 u)
	{
		return u / float((1 << 15) - 1);
	}

	float3 DecodeSNorm3(int16_t3 u)
	{
		return u / float((1 << 15) - 1);
	}

	float4 DecodeSNorm4(int16_t4 u)
	{
		return u / float((1 << 15) - 1);
	}
	
	// Encodes 3D unit vector using octahedral mapping.
	// Ref: https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
	float2 EncodeUnitVector(float3 n)
	{
		float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
		return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * SignNotZero(p)) : p;
	}

	float3 DecodeUnitVector(float2 u)
	{
		// https://twitter.com/Stubbesaurus/status/937994790553227264
		float3 n = float3(u.x, u.y, 1.0f - abs(u.x) - abs(u.y));
		float t = saturate(-n.z);
		//n.xy += n.xy >= 0.0f ? -t : t;
		n.xy += select(n.xy >= 0.0f, -t, t);

		return normalize(n);
	}

	int16_t2 EncodeOct16(float3 n)
	{
		float2 u = EncodeUnitVector(n);
		int16_t2 e = EncodeAsSNorm2(u);

		return e;
	}

	float3 DecodeOct16(int16_t2 e)
	{
		float2 u = DecodeSNorm2(e);
		float3 n = float3(u.x, u.y, 1.0f - abs(u.x) - abs(u.y));
		float t = saturate(-n.z);
		n.xy += select(n.xy >= 0.0f, -t, t);

		return normalize(n);
	}
	
	// Octahedral encoding for unit vector in upper hemisphere (n.y >= 0).
	float2 EncodeUnitHemisphereVector(float3 n)
	{
		float2 p = n.xz / (abs(n.x) + abs(n.y) + abs(n.z));
		return p;
	}

	float3 DecodeUnitHemisphereVector(float2 u)
	{
		float3 n = float3(u.x, 1.0f - abs(u.x) - abs(u.y), u.y);
		return normalize(n);
	}

	float Luminance(float3 linearRGB)
	{
		return dot(float3(0.2126f, 0.7152f, 0.0722f), linearRGB);
	}

	float3 LinearTosRGB(float3 color)
	{
		float3 sRGBLo = color * 12.92;
		float3 sRGBHi = (pow(saturate(color), 1.0f / 2.4f) * 1.055) - 0.055;
		float3 sRGB = select((color <= 0.0031308f), sRGBLo, sRGBHi);

		return sRGB;
	}

	float3 sRGBToLinear(float3 color)
	{
		float3 linearRGBLo = color / 12.92f;
		float3 linearRGBHi = pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f);
		float3 linearRGB = select(color <= 0.0404499993f, linearRGBLo, linearRGBHi);

		return linearRGB;
	}
			
	float3 LinearToYCbCr(float3 x)
	{
		float3x3 M = float3x3(0.2126, 0.7152, 0.0722,
								-0.1146, -0.3854, 0.5,
								0.5, -0.4542, -0.0458);
		return mul(M, x);
	}
	
	float2 UnpackRG(uint rg)
	{
		float2 ret;
		ret.x = float(rg & 0xff) / 255.0f;
		ret.y = float((rg >> 8) & 0xff) / 255.0f;
		
		return ret;
	}
	
	float3 UnpackRGB(uint rgb)
	{
		float3 ret;
		ret.x = float(rgb & 0xff) / 255.0f;
		ret.y = float((rgb >> 8) & 0xff) / 255.0f;
		ret.z = float((rgb >> 16) & 0xff) / 255.0f;
		
		return ret;
	}

	float4 UnpackRGBA(uint rgba)
	{
		float4 ret;
		ret.x = float(rgba & 0xff) / 255.0f;
		ret.y = float((rgba >> 8) & 0xff) / 255.0f;
		ret.z = float((rgba >> 16) & 0xff) / 255.0f;
		ret.w = float((rgba >> 24) & 0xff) / 255.0f;
		
		return ret;
	}
}

#endif