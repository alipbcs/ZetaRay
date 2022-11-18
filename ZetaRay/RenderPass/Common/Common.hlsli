#ifndef COMMON_H
#define COMMON_H

struct Vertex
{
	float3 PosL;
	float3 NormalL;
	float2 TexUV;
	float3 TangentU;
};

namespace Common
{
	// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
	// Ref: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
	template<typename T>
	float3 SampleTextureCatmullRom_5Tap(Texture2D<T> tex, SamplerState linearSampler, float2 uv, float2 texSize)
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
}

#endif // COMMON_H