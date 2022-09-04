#ifndef COMMON_H
#define COMMON_H

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

struct Vertex
{
	float3 PosL;
	float3 NormalL;
	float2 TexUV;
	float3 TangentU;
};

// convert NDC-space depth to view-space depth
float ComputeLinearDepth(float zNDC, float near, float far)
{
	return (near * far) / (far - (far - near) * zNDC);
}

float4 ComputeLinearDepth(float4 zNDC, float near, float far)
{
	return (near * far) / (far - (far - near) * zNDC);
}

float ComputeLinearDepthReverseZ(float zNDC, float near)
{
	return near / zNDC;
}

float4 ComputeLinearDepthReverseZ(float4 zNDC, float near)
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

float3 WorldPosFromUV(float2 uv, float linearDepth, float tanHalfFOV, float aspectRatio, float3x4 viewInv, 
	float2 jitter)
{
	const float2 posNDC = NDCFromUV(uv);
	const float xView = posNDC.x * tanHalfFOV * aspectRatio - jitter.x;
	const float yView = posNDC.y * tanHalfFOV - jitter.y;
	float3 posW = float3(xView, yView, 1.0f) * linearDepth;
	posW = mul(viewInv, float4(posW, 1.0f));
	
	return posW;
}

float3 WorldPosFromScreenSpacePos(float2 posSS, float2 screenDim, float linearDepth, float tanHalfFOV,
	float aspectRatio, float3x4 viewInv, float2 jitter)
{
	const float2 uv = UVFromScreenSpace(posSS, screenDim);
	const float2 posNDC = NDCFromUV(uv);
	const float xView = posNDC.x * tanHalfFOV * aspectRatio - jitter.x;
	const float yView = posNDC.y * tanHalfFOV - jitter.y;
	float3 posW = float3(xView, yView, 1.0f) * linearDepth;
	posW = mul(viewInv, float4(posW, 1.0f));
	
	return posW;
}

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
	return (1.0f - abs(v.yx)) * (v.xy >= 0.0f ? 1.0f : -1.0f);
}
 
half2 EncodeUnitNormalAsHalf2(float3 n)
{
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0f ? n.xy : OctWrap(n.xy);
	n.xy = n.xy * 0.5f + 0.5f;
	
	return half2(n.xy);
}

uint EncodeUnitNormalAsUint(float3 n)
{
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0f ? n.xy : OctWrap(n.xy);
	n.xy = n.xy * 0.5f + 0.5f;
	
	uint encoded = f32tof16(n.y) << 16 | f32tof16(n.x);

	return encoded;
}
 
float3 DecodeUnitNormalFromHalf2(float2 u)
{
	float2 f = u * 2.0f - 1.0f;
 
    // https://twitter.com/Stubbesaurus/status/937994790553227264
	float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0f ? -t : t;
	
	return normalize(n);
}

float3 DecodeUnitNormalFromUint(uint encoded)
{
	float2 u = float2(f16tof32(encoded), f16tof32(encoded >> 16));
	float2 f = u * 2.0f - 1.0f;
 
    // https://twitter.com/Stubbesaurus/status/937994790553227264
	float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0f ? -t : t;
	
	return normalize(n);
}

// computes smallest floating point f' larget than f that can be represented
// using a 32-bit float (precision changes depending of exponent)
float NextFloat32Up(float f)
{
	uint u = asuint(f);
	u = f >= 0 ? u + 1 : u - 1;
	
	return asfloat(u);
}

float2 NextFloat32Up(float2 f)
{
	uint2 u = asuint(f);
	u = (f >= 0) * (u + 1) + (f < 0) * (u - 1);
	
	return asfloat(u);
}

float3 NextFloat32Up(float3 f)
{
	uint3 u = asuint(f);
	u = (f >= 0) * (u + 1) + (f < 0) * (u - 1);
	
	return asfloat(u);
}

float4 NextFloat32Up(float4 f)
{
	uint4 u = asuint(f);
	u = (f >= 0) * (u + 1) + (f < 0) * (u - 1);
	
	return asfloat(u);
}

float NextFloat32Down(float x)
{
	uint u = asuint(x);
	u = x >= 0 ? u - 1 : u + 1;
	
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

float LuminanceFromLinearRGB(float3 linearRGB)
{
	return dot(float3(0.2126f, 0.7152f, 0.0722f), linearRGB);
}

half LuminanceFromLinearRGB(half3 linearRGB)
{
	return dot(half3(0.2126h, 0.7152h, 0.0722h), linearRGB);
}

// Ref: "Moving Frostbite to Physically Based Rendering"
float3 LinearTosRGB(float3 color)
{
	float3 sRGBLo = color * 12.92;
	float3 sRGBHi = (pow(saturate(color), 1.0f / 2.4f) * 1.055) - 0.055;
	float3 sRGB = (color <= 0.0031308) ? sRGBLo : sRGBHi;
	
	return sRGB;
}

// Ref: "Moving Frostbite to Physically Based Rendering"
float3 sRGBToLinear(float3 color)
{
	float3 linearRGBLo = color / 12.92f;
	float3 linearRGBHi = pow(max((color + 0.055f) / 1.055f, 0.0f), 2.4f);
	float3 linearRGB = color <= 0.04045f ? linearRGBLo : linearRGBHi;

	return linearRGB;
}

bool IsInRange(int2 pos, int2 dim)
{
	return pos.x >= 0 && pos.x < dim.x && pos.y >= 0 && pos.y < dim.y;
}

bool IsInRange(int16_t2 pos, int16_t2 dim)
{
	return pos.x >= 0 && pos.x < dim.x && pos.y >= 0 && pos.y < dim.y;
}

bool IsInRange(uint2 pos, uint2 dim)
{
	return pos.x < dim.x && pos.y < dim.y;
}

bool IsInRange(float2 pos, float2 dim)
{
	return pos.x >= 0 && pos.x < dim.x && pos.y >= 0 && pos.y < dim.y;
}

float3 SphericalToCartesian(float r, float cosTheta, float phi)
{
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
	float cosPhi = cos(phi);
	float sinPhi = sqrt(1.0f - cosPhi * cosPhi);
	
	return float3(r * sinTheta * cosPhi, r * cosTheta, r * sinTheta * sinPhi);
}

float3 TangentSpaceNormalToWorldSpace(float2 bumpNormal2, float3 tangentW, float3 normalW, float scale)
{
    // graham-schmidt normalization
	normalW = normalize(normalW);
	tangentW = normalize(tangentW - dot(tangentW, normalW) * normalW);

    // TBN coordiante basis
	float3 bitangentW = cross(normalW, tangentW);
	float3x3 TangentSpaceToWorld = float3x3(tangentW, bitangentW, normalW);

	float3 bumpNormal = float3(2.0f * bumpNormal2 - 1.0f, 0.0f);
	bumpNormal.z = sqrt(saturate(1.0f - dot(bumpNormal, bumpNormal)));
	// scaledNormal = normalize<sampled normal texture value> * 2.0 - 1.0) * vec3(<normal scale>, <normal scale>, 1.0.
	float3 scaledBumpNormal = normalize(bumpNormal * float3(scale, scale, 1.0f));

	return mul(bumpNormal, TangentSpaceToWorld);
}

float3 TangentSpaceNormalToWorldSpace(float3 bumpNormal, float3 tangentW, float3 normalW, float scale)
{
    // graham-schmidt normalization
	normalW = normalize(normalW);
	tangentW = normalize(tangentW - dot(tangentW, normalW) * normalW);

    // TBN coordiante basis
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

// Ref: https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
// input in [-1, 1] and output in [0, PI]
float ArcCos(float x)
{
	float xAbs = abs(x);
	float res = ((-0.0206453f * xAbs + 0.0764532f) * xAbs + -0.21271f) * xAbs + 1.57075f;
	res *= sqrt(1.0f - xAbs);

	return (x >= 0) ? res : PI - res;
}

// Ref: https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
// input [-infinity, infinity] and output [-PI/2, PI/2]
float ArcTan(float x)
{
	float xAbs = abs(x);
	float t0 = (xAbs < 1.0f) ? xAbs : 1.0f / xAbs;
	float t1 = t0 * t0;
	float poly = 0.0872929f;
	poly = -0.301895f + poly * t1;
	poly = 1.0f + poly * t1;
	poly = poly * t0;
	poly = (xAbs < 1.0f) ? poly : PI_DIV_2 - poly;

	return (x < 0.0f) ? -poly : poly;
}

// dPdx(y) can be estimated with ddx(y)(PosW)
// dNdx(y) can be estimated with ddx(y)(NormalW)
float ComputeSurfaceSpreadAngle(float3 dPdx, float3 dPdy, float3 dNdx, float3 dNdy, float k1, float k2)
{
	float phi = length(dNdx + dNdy);
		
	// dot(ddxPos, ddxNormal) > 0 --> surface is convex since position and normal are changing in the same direction
	// dot(ddxPos, ddxNormal) < 0 --> surface is concave since position and normal are changing in the opposite direction
	float beta = 2.0f * k1 * sign(dot(dPdx, dNdx) + dot(dPdy, dNdy)) + k2;
		
	return beta;
}

// Compute surface area of a pixel at distance d from eye 
// solid angle subtended by a pixel at distance z is the same as solid angle 
// subtended by that pixel at z=near plane
float SurfaceAreaForPixel(float xNdc, float yNdc, float linearZ, float3 viewNormal, float fovy, float near, uint dimX, uint dimY)
{
	float t = tan(0.5f * fovy);
	float r = (float) dimX / dimY;

	// reconstruct view-space position
	float xView = xNdc * r * t * linearZ;
	float yView = yNdc * t * linearZ;
	
	float3 toEye = float3(-xView, -yView, -linearZ);
	float cos_theta = dot(normalize(toEye), viewNormal);
	
	// clamp cos(theta) from becoming too small for grazing angles
	cos_theta = max(0.2f, cos_theta);
		
	// area of pixel at z=near plane
	float areaN = (4.0f * near * near * t * t) / (dimY * dimY);
	float areaD = (areaN * linearZ * linearZ) / cos_theta;
	
	return areaD;
}

// pixelSpreadAngle is computed as Eq. (30) in Ray Tracing Gems chapter 20
// z in linear depth for pixel
// ddxy is partial derivative of linear depth with respect to left/right and up/down pixels
float2 SurfaceDimForPixel(float z, float2 ddxy, float pixelSpreadAngle)
{
	// small angle approximation (tan(a) ~ a)
	float width = pixelSpreadAngle * z;
	
	return float2(ddxy.x * ddxy.x + width * width, ddxy.y * ddxy.y + width * width);
}

// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
// Ref: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
float4 SampleTextureCatmullRom_9Tap(in Texture2D<float4> tex, in SamplerState linearSampler, in float2 uv, in float2 texSize)
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

	float4 result = 0.0f;
	result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos0.y), 0.0f) * w0.x * w0.y;
	result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos0.y), 0.0f) * w12.x * w0.y;
	result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos0.y), 0.0f) * w3.x * w0.y;

	result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos12.y), 0.0f) * w0.x * w12.y;
	result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos12.y), 0.0f) * w12.x * w12.y;
	result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos12.y), 0.0f) * w3.x * w12.y;

	result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos3.y), 0.0f) * w0.x * w3.y;
	result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos3.y), 0.0f) * w12.x * w3.y;
	result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos3.y), 0.0f) * w3.x * w3.y;

	return result;
}

// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
// Ref: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
float3 SampleTextureCatmullRom_5Tap(in Texture2D<float4> tex, in SamplerState linearSampler, in float2 uv, in float2 texSize)
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

#endif // COMMON_H