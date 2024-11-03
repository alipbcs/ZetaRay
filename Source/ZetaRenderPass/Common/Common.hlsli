#ifndef COMMON_H
#define COMMON_H

// 28 bytes
struct Vertex
{
    float3 PosL;
    float2 TexUV;
    uint16_t2 NormalL;
    uint16_t2 TangentU;
};

namespace Common
{
    // Samples texture with Catmull-Rom filtering, using 5 texture fetches instead of 16.
    // Ref: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
    float3 SampleTextureCatmullRom_5Tap(Texture2D<float4> tex, SamplerState linearSampler, float2 uv, float2 texSize)
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

    // Samples texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
    // Ref: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
    float3 SampleTextureCatmullRom(Texture2D<float4> tex, SamplerState linearSampler, float2 uv, float2 texSize)
    {
        float2 samplePos = uv * texSize;
        float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;

        float2 f = samplePos - texPos1;

        float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
        float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
        float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
        float2 w3 = f * f * (-0.5f + 0.5f * f);

        float2 w12 = w1 + w2;
        float2 offset12 = w2 / (w1 + w2);

        float2 texPos0 = texPos1 - 1;
        float2 texPos3 = texPos1 + 2;
        float2 texPos12 = texPos1 + offset12;

        texPos0 /= texSize;
        texPos3 /= texSize;
        texPos12 /= texSize;

        float3 result = 0.0f;
        result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos0.y), 0.0f).xyz * w0.x * w0.y;
        result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos0.y), 0.0f).xyz * w12.x * w0.y;
        result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos0.y), 0.0f).xyz * w3.x * w0.y;
        
        result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos12.y), 0.0f).xyz * w0.x * w12.y;
        result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos12.y), 0.0f).xyz * w12.x * w12.y;
        result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos12.y), 0.0f).xyz * w3.x * w12.y;

        result += tex.SampleLevel(linearSampler, float2(texPos0.x, texPos3.y), 0.0f).xyz * w0.x * w3.y;
        result += tex.SampleLevel(linearSampler, float2(texPos12.x, texPos3.y), 0.0f).xyz * w12.x * w3.y;
        result += tex.SampleLevel(linearSampler, float2(texPos3.x, texPos3.y), 0.0f).xyz * w3.x * w3.y;
        
        return result;
    }

    // Swizzle thread groups for better L2-cache behavior. Works by breaking the 
    // image into tiles of dimension (N, dispatchDim.y).
    // dispatchDim: Same as DispatchThreads() arguments. Must be a power of two.
    // Gid: SV_GroupID
    // GTid: SV_GroupThreadID
    // groupDim: e.g. [numthreads(8, 8, 1)] -> (8, 8)
    // N: Number of horizontal blocks in each tile, common values are 8, 16, 32. Must 
    // be a power of two.
    // Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
    void SwizzleGroupID(in uint2 Gid, in uint2 GTid, uint2 groupDim, in uint2 dispatchDim, in uint N,
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

    uint2 SwizzleThreadGroup(uint3 DTid, uint3 Gid, uint3 GTid, uint2 groupDim, uint dispatchDimX, uint tileWidth,
        uint log2TileWidth, uint numGroupsInTile, out uint2 swizzledGid)
    {
        const uint groupIDFlattened = Gid.y * dispatchDimX + Gid.x;
        const uint tileID = groupIDFlattened / numGroupsInTile;
        const uint groupIDinTileFlattened = groupIDFlattened % numGroupsInTile;

        // TileWidth is a power of 2 for all tiles except possibly the last one
        const uint numFullTiles = dispatchDimX / tileWidth; // floor(DispatchDimX / TileWidth
        const uint numGroupsInFullTiles = numFullTiles * numGroupsInTile;

        uint2 groupIDinTile;
        if (groupIDFlattened >= numGroupsInFullTiles)
        {
            // DispatchDimX & NumGroupsInTile
            const uint lastTileDimX = dispatchDimX - tileWidth * numFullTiles;
            groupIDinTile = uint2(groupIDinTileFlattened % lastTileDimX, groupIDinTileFlattened / lastTileDimX);
        }
        else
        {
            groupIDinTile = uint2(
                groupIDinTileFlattened & (tileWidth - 1),
                groupIDinTileFlattened >> log2TileWidth);
        }

        const uint swizzledGidFlattened = groupIDinTile.y * dispatchDimX + tileID * tileWidth + groupIDinTile.x;
        swizzledGid = uint2(swizzledGidFlattened % dispatchDimX, swizzledGidFlattened / dispatchDimX);
        const uint2 swizzledDTid = swizzledGid * groupDim + GTid.xy;
    
        return swizzledDTid;
    }

    bool IsFlagSet(uint32_t bitmask, uint32_t flag)
    {
        return (bitmask & flag) == flag;
    }
}

#endif // COMMON_H