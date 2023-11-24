#ifndef LVG_H
#define LVG_H

#include "LightSource.hlsli"

namespace LVG
{
    uint FlattenVoxelIndex(uint3 voxelIdx, uint3 gridDim)
    {
        return voxelIdx.z * gridDim.x * gridDim.y + voxelIdx.y * gridDim.x + voxelIdx.x;
    }

    float3 VoxelCenter(int3 voxelIdx, int3 gridDim, float3 voxelExtents, float3x4 viewInv, float offset_y = 0)
    {
        // e.g. when dim = 8, map
        //  0  1  2  3  4  5  6  7
        // to
        // -3 -2 -1  0  0  1  2  3
        int3 dimDiv2 = gridDim >> 1;
        int3 voxelIdxCamSpace = voxelIdx - dimDiv2;
        voxelIdxCamSpace += voxelIdx < dimDiv2;
        // voxel space Y points in the opposite direction of camera space Y
        voxelIdxCamSpace.y *= -1;

        float3 corner = voxelIdxCamSpace * 2 * voxelExtents;
        float3 s = Math::SignNotZero(voxelIdxCamSpace);
        float3 centerV = corner + voxelExtents * s;
        centerV.y += offset_y;
        float3 centerW = mul(viewInv, float4(centerV, 1));
        
        return centerW;
    }

    bool MapPosToVoxel(float3 pos, int3 gridDim, float3 voxelExtents, float3x4 view, out int3 idx, float offset_y = 0)
    {
        float3 posV = mul(view, float4(pos, 1));
        posV.y -= offset_y;

        int3 dimDiv2 = gridDim >> 1;
        float3 voxel = floor(abs(posV) / (2 * voxelExtents));

        if(any(voxel >= dimDiv2))
            return false;
        
        voxel *= Math::SignNotZero(posV);
        voxel.y *= -1;

        int3 voxelIdx = int3(voxel) + dimDiv2;
        voxelIdx -= int3(posV.x < 0, posV.y >= 0, posV.z < 0);
        idx = voxelIdx;

        return true;
    }

    bool Sample(float3 pos, uint3 gridDim, float3 voxelExtents, uint numLightsPerVoxel, float3x4 view, 
        StructuredBuffer<RT::VoxelSample> g_voxel, out RT::VoxelSample s, inout RNG rng, float offset_y = 0, 
        bool jitter = true)
    {
        float3 posJittered = jitter ? pos + (rng.Uniform3D() * 2 - 1) * voxelExtents : pos;
        
        int3 voxelIdx;
        if(!MapPosToVoxel(posJittered, gridDim, voxelExtents, view, voxelIdx, offset_y))
            return false;
        
        uint start = FlattenVoxelIndex(voxelIdx, gridDim) * numLightsPerVoxel;
		uint u = rng.UniformUintBounded_Faster(numLightsPerVoxel);
        s = g_voxel[start + u];
        
        return true;
    }
}

#endif