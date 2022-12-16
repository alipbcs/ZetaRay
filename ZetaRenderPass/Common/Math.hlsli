#ifndef MATH_H
#define MATH_H

#define PI					3.141592654f
#define TWO_PI				6.283185307f
#define PI_DIV_2			1.570796327f
#define PI_DIV_4			0.7853981635f
#define ONE_DIV_PI			0.318309886f
#define ONE_DIV_TWO_PI		0.159154943f
#define ONE_DIV_FOUR_PI		0.079577472f
#define TWO_DIV_PI			0.636619772f
#define FLT_MIN				1.175494351e-38 
#define FLT_MAX				3.402823466e+38 

namespace Math
{
	// Returns whether pos is in [0, dim)
	template<typename T>
	bool IsWithinBoundsExc(T pos, T dim)
	{
		return all(pos >= 0) && all(pos < dim);
	}

	// Returns whether pos is in [0, dim]
	template<typename T>
	bool IsWithinBoundsInc(T pos, T dim)
	{
		return all(pos >= 0) && all(pos <= dim);
	}
	
	// computes smallest floating point f' such that f' > f
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
	
	// Ref: https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
	// input in [-1, 1] and output in [0, PI]
	float ArcCos(float x)
	{
		float xAbs = abs(x);
		float res = ((-0.0206453f * xAbs + 0.0764532f) * xAbs + -0.21271f) * xAbs + 1.57075f;
		res *= sqrt(1.0f - xAbs);

		return (x >= 0) ? res : PI - res;
	}
	
	float3 SphericalToCartesian(float r, float cosTheta, float phi)
	{
		float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
		float cosPhi = cos(phi);
		float sinPhi = sqrt(1.0f - cosPhi * cosPhi);
	
		return float3(r * sinTheta * cosPhi, r * cosTheta, r * sinTheta * sinPhi);
	}

	float2 SphericalFromCartesian(float3 w)
	{
		float2 thetaPhi;
		
		// x = sin(theta) * cos(phi)
		// y = cos(theta)
		// z = sin(theta) * sin(phi)
		thetaPhi.x = ArcCos(w.y); // [-PI, +PI]
		thetaPhi.y = atan2(w.z, w.x);
		thetaPhi.y = thetaPhi.y < 0.0f ? thetaPhi.y + PI : thetaPhi.y; // [0, 2 * PI]
		
		return thetaPhi;
	}
	
	// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
	// Ref: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
	template<typename T>
	float3 SampleTextureCatmullRom_5Tap(in Texture2D<T> tex, in SamplerState linearSampler, in float2 uv, in float2 texSize)
	{
		// We're going to sample a a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
		// down the sample location to get the exact center of our "starting" texel. The starting texel will be at
		// location [1, 1] in the grid, where [0, 0] is the top left corner.
		float2 samplePos = uv * texSize;
		float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;

		// Compute the fractional offset from our starting texel to our original sample location, which we'll
		// feed into the Catmull-Rom spline function to get our filter weights.
		float2 f = samplePos - texPos1;

		// Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
		// These equations are pre-expanded based on our knowledge of where the texels will be located,
		// which lets us avoid having to evaluate a piece-wise function.
		float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
		float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
		float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
		float2 w3 = f * f * (-0.5f + 0.5f * f);

		// Work out weighting factors and sampling offsets that will let us use bilinear filtering to
		// simultaneously evaluate the middle 2 samples from the 4x4 grid.
		float2 w12 = w1 + w2;
		float2 offset12 = w2 / (w1 + w2);

	    // Compute the final UV coordinates we'll use for sampling the texture
		float2 texPos0 = texPos1 - 1;
		float2 texPos3 = texPos1 + 2;
		float2 texPos12 = texPos1 + offset12;

		texPos0 /= texSize;
		texPos3 /= texSize;
		texPos12 /= texSize;

		float3 result = 0.0f;
		result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos0.y), 0.0f).xyz * w12.x * w0.y;

		result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos12.y), 0.0f).xyz * w0.x * w12.y;
		result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos12.y), 0.0f).xyz * w12.x * w12.y;
		result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos12.y), 0.0f).xyz * w3.x * w12.y;

		result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos3.y), 0.0f).xyz * w12.x * w3.y;

		return result;
	}

	// Breaks the image into tiles of dimesnion (N, dispatchDim.y)
	// dispatchDim: same as DispatchThreads() arguments. Must be a power of 2.
	// Gid: SV_GroupID
	// GTid: SV_GroupThreadID
	// groupDim: e.g. [numthreads(8, 8, 1)] -> (8, 8)
	// N: number of horizontal blocks in each tile, common values are 8, 16, 32. Corresponds
	// to maximum horizontal extent which threads acceess image value (e.g. accessing pixel
	// values in a block which N horizontal blocks further). N must be a power of 2.
	void SwizzleGroupID(in uint2 Gid, in uint2 GTid, uint2 groupDim, in uint2 dispatchDim, in int N,
		out uint2 swizzledDTid, out uint2 swizzledGid)
	{
		const uint groupIDFlattened = Gid.y * dispatchDim.x + Gid.x;
		const uint numGroupsInTile = N * dispatchDim.y;
		const uint tileID = groupIDFlattened / numGroupsInTile;
		const uint groupIDinTileFlattened = groupIDFlattened % numGroupsInTile;
		const uint2 groupIDinTile = uint2(groupIDinTileFlattened % N, groupIDinTileFlattened / N);
		const uint swizzledGidx = groupIDinTile.y * dispatchDim.x + tileID * N + groupIDinTile.x;
	
		swizzledGid = uint2(swizzledGidx % dispatchDim.x, swizzledGidx / dispatchDim.x);
		swizzledDTid = swizzledGid * groupDim + GTid;
	}
	
	namespace Transform
	{
		template<typename FVec>
		FVec LinearDepthFromNDC(FVec zNDC, float near)
		{
			return near / zNDC;
		}
		
		float2 NDCFromUV(float2 uv)
		{
			float2 posNDC = uv * 2.0f - 1.0f;
			posNDC.y = -posNDC.y;
	
			return posNDC;
		}

		float2 UVFromNDC(float2 posNDC)
		{
			float x = 0.5f * posNDC.x + 0.5f;
			float y = -0.5f * posNDC.y + 0.5f;
	
			return float2(x, y);
		}

		float2 ScreenSpaceFromNDC(float2 posNDC, float2 screenDim)
		{
			// [-1, 1] * [-1, 1] -> [0, 1] * [0, 1]
			float2 posSS = posNDC * float2(0.5f, -0.5f) + 0.5f;
			posSS *= screenDim;

			return posSS;
		}

		float2 UVFromScreenSpace(uint2 posSS, float2 screenDim)
		{
			float2 uv = float2(posSS) + 0.5f;
			uv /= screenDim;
	
			return uv;
		}

		float3 WorldPosFromUV(float2 uv, float linearDepth, float tanHalfFOV, float aspectRatio, float3x4 viewInv)
		{
			const float2 posNDC = NDCFromUV(uv);
			const float xView = posNDC.x * tanHalfFOV * aspectRatio;
			const float yView = posNDC.y * tanHalfFOV;
			float3 posW = float3(xView, yView, 1.0f) * linearDepth;
			posW = mul(viewInv, float4(posW, 1.0f));
	
			return posW;
		}

		float3 WorldPosFromScreenSpace(float2 posSS, float2 screenDim, float linearDepth, float tanHalfFOV,
			float aspectRatio, float3x4 viewInv)
		{
			const float2 uv = UVFromScreenSpace(posSS, screenDim);
			const float2 posNDC = NDCFromUV(uv);
			const float xView = posNDC.x * tanHalfFOV * aspectRatio;
			const float yView = posNDC.y * tanHalfFOV;
			float3 posW = float3(xView, yView, 1.0f) * linearDepth;
			posW = mul(viewInv, float4(posW, 1.0f));
	
			return posW;
		}
		
		float3 ViewPosFromUV(float2 uv, float linearDepth, float tanHalfFOV, float aspectRatio)
		{
			const float2 posNDC = NDCFromUV(uv);
			const float xView = posNDC.x * tanHalfFOV * aspectRatio;
			const float yView = posNDC.y * tanHalfFOV;
			float3 posV = float3(xView, yView, 1.0f) * linearDepth;
	
			return posV;
		}
		
		float3 TangentSpaceToWorldSpace(float2 bumpNormal2, float3 tangentW, float3 normalW, float scale)
		{
		    // graham-schmidt normalization
			normalW = normalize(normalW);
			tangentW = normalize(tangentW - dot(tangentW, normalW) * normalW);

			// TBN coordiante system
			float3 bitangentW = cross(normalW, tangentW);
			float3x3 TangentSpaceToWorld = float3x3(tangentW, bitangentW, normalW);

			float3 bumpNormal = float3(2.0f * bumpNormal2 - 1.0f, 0.0f);
			bumpNormal.z = sqrt(saturate(1.0f - dot(bumpNormal, bumpNormal)));
			// scaledNormal = normalize<sampled normal texture value> * 2.0 - 1.0) * vec3(<normal scale>, <normal scale>, 1.0.
			float3 scaledBumpNormal = normalize(bumpNormal * float3(scale, scale, 1.0f));

			return mul(bumpNormal, TangentSpaceToWorld);
		}

		float3 TangentSpaceToWorldSpace(float3 bumpNormal, float3 tangentW, float3 normalW, float scale)
		{
			// graham-schmidt normalization
			normalW = normalize(normalW);
			tangentW = normalize(tangentW - dot(tangentW, normalW) * normalW);

			// TBN coordiante system
			float3 bitangentW = cross(normalW, tangentW);
			float3x3 TangentSpaceToWorld = float3x3(tangentW, bitangentW, normalW);

			bumpNormal = 2.0f * bumpNormal - 1.0f;
			// scaledNormal = normalize<sampled normal texture value> * 2.0 - 1.0) * vec3(<normal scale>, <normal scale>, 1.0.
			float3 scaledBumpNormal = normalize(bumpNormal * float3(scale, scale, 1.0f));

			return mul(bumpNormal, TangentSpaceToWorld);
		}
		
		// Reference: https://blog.selfshadow.com/2011/10/17/perp-vectors/
		// returns a vector that is perpendicular to u (assumed to be normalized)
		float3 GetPerpendicularVector(float3 u)
		{
			float3 a = abs(u);

			uint xm = ((a.x - a.y) < 0 && (a.x - a.z) < 0) ? 1 : 0;
			uint ym = (a.y - a.z) < 0 ? (1 ^ xm) : 0;
			uint zm = 1 ^ (xm | ym);

			return cross(u, float3(xm, ym, zm));
		}
		
		// Quaternion that rotates u to v
		float4 QuaternionFromVectors(float3 u, float3 v)
		{
			float unormvorm = sqrt(dot(u, u) * dot(v, v));
			float udotv = dot(u, v);
			float4 q;
	
			if (unormvorm - udotv > 1e-6 * unormvorm)
			{
				float3 imaginary = cross(v, u);
				float real = udotv + unormvorm;
		
				q = normalize(float4(imaginary, real));
			}
			else
			{
				float3 p = GetPerpendicularVector(normalize(u));

				// rotate 180/2 = 90 degrees
				q = float4(p, 0.0f);
		
				// p is already normalized so no need to normalize q
			}
	
			return q;
		}

		// Quaternion that rotates u to v
		// u and v are assumed to be normalized
		float4 QuaternionFromUnitVectors(float3 u, float3 v)
		{
			float udotv = dot(u, v);
			float4 q;
	
			// u == -v is a singularity
			if (udotv < 1.0f - 1e-6)
			{
				float3 imaginary = cross(v, u);
				float real = udotv + 1.0f;
				q = normalize(float4(imaginary, real));
			}
			else
			{
				float3 p = GetPerpendicularVector(u);

				// rotate 180/2 = 90 degrees
				q = float4(p, 0.0f);
		
				// p is already normalized, so no need to normalize q
			}
	
			return q;
		}

		// Quaternion that rotates Y to u
		// u is assumed to be normalized
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
				// rotate 180/2 = 90 degrees around X-axis (Z-axis works too since both are perperndicular to Y)
				q = float4(1.0f, 0.0f, 0.0f, 0.0f);
		
				// no need to normalize q
			}
	
			return q;
		}

		// rotate u using rotation quaternion q by computing q * u * q*
		// * is quaternion multiplication
		// q* is the conjugate of q
		// Ref: https://gamedev.stackexchange.com/questions/28395/rotating-vector3-by-a-quaternion
		float3 RotateVector(float3 u, float4 q)
		{
			float3 imaginary = q.xyz;
			float real = q.w;
	
			float3 rotated = 2.0f * dot(imaginary, u) * imaginary +
					(real * real - dot(imaginary, imaginary)) * u +
					2.0f * real * cross(imaginary, u);

			return rotated;
		}
	}

	namespace Encoding
	{
		// encode [0, 1] float as 8-bit unsigned integer (unorm)
		// reference: "A Survey of Efficient Representations for Independent UnitVectors"
		uint Encode01FloatAsUNORM(float r)
		{
			return round(clamp(r, 0.0f, 1.0f) * 255);
		}

		// decode 8-bit unsigned integer (unorm) to [0, 1] float
		float DecodeUNORMTo01Float(uint i)
		{
			return (i & 0xff) / 255.0f;
		}

		// encoded unit float3 normal as float2 in [0, 1]
		// reference: https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
		float2 OctWrap(float2 v)
		{
			//return (1.0f - abs(v.yx)) * (v.xy >= 0.0f ? 1.0f : -1.0f);
			return (1.0f - abs(v.yx)) * select(v.xy >= 0.0f, 1.0f, -1.0f);
		}
 
		half2 EncodeUnitNormalAsHalf2(float3 n)
		{
			n /= (abs(n.x) + abs(n.y) + abs(n.z));
			n.xy = n.z >= 0.0f ? n.xy : OctWrap(n.xy);
			n.xy = n.xy * 0.5f + 0.5f;
	
			return half2(n.xy);
		}
 
		float3 DecodeUnitNormalFromHalf2(float2 u)
		{
			float2 f = u * 2.0f - 1.0f;
 
		    // https://twitter.com/Stubbesaurus/status/937994790553227264
			float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
			float t = saturate(-n.z);
			//n.xy += n.xy >= 0.0f ? -t : t;
			n.xy += select(n.xy >= 0.0f, -t, t);
	
			return normalize(n);
		}		
	}

	namespace Color
	{
		float LuminanceFromLinearRGB(float3 linearRGB)
		{
			return dot(float3(0.2126f, 0.7152f, 0.0722f), linearRGB);
		}

		half LuminanceFromLinearRGB(half3 linearRGB)
		{
			return dot(half3(0.2126h, 0.7152h, 0.0722h), linearRGB);
		}
		
		// Ref: S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
		float3 LinearTosRGB(float3 color)
		{
			float3 sRGBLo = color * 12.92;
			float3 sRGBHi = (pow(saturate(color), 1.0f / 2.4f) * 1.055) - 0.055;
			//float3 sRGB = (color <= 0.0031308) ? sRGBLo : sRGBHi;
			float3 sRGB = select((color <= 0.0031308f), sRGBLo, sRGBHi);
	
			return sRGB;
		}

		// Ref: S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
		float3 sRGBToLinear(float3 color)
		{
			float3 linearRGBLo = color / 12.92f;
			float3 linearRGBHi = pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f);
			//float3 linearRGB = color <= 0.04045f ? linearRGBLo : linearRGBHi;
			float3 linearRGB = select(color <= 0.0404499993f, linearRGBLo, linearRGBHi);

			return linearRGB;
		}
	}
}

#endif // COMMON_H