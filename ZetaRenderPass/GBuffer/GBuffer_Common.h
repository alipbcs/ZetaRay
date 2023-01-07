#ifndef GBUFFER_H
#define GBUFFER_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define BUILD_NO_CULL_THREAD_GROUP_SIZE_X 64
#define BUILD_OCC_CULL_THREAD_GROUP_SIZE_X 64

struct DRAW_INDEXED_ARGUMENTS
{
    uint32_t IndexCountPerInstance;
    uint32_t InstanceCount;
    uint32_t StartIndexLocation;
    int BaseVertexLocation;
    uint32_t StartInstanceLocation;
};

struct DRAW_ARGUMENTS
{
    uint32_t VertexCountPerInstance;
    uint32_t InstanceCount;
    uint32_t StartVertexLocation;
    uint32_t StartInstanceLocation;
};

struct CommandSig
{
    uint32_t RootConstant;
    DRAW_INDEXED_ARGUMENTS DrawArgs;
};

struct HLSL_AABB
{
    float3_ Center;
    float3_ Extents;
};

struct MeshInstance
{
    row_major float3x4_ CurrWorld;
    row_major float3x4_ PrevWorld;

    uint32_t IndexCount;
    uint32_t BaseVtxOffset;
    uint32_t BaseIdxOffset;
    uint16_t IdxInMatBuff;
    uint16_t IsDoubleSided;
    uint32_t VisibilityIdx;
    HLSL_AABB BoundingBox;

    float pad;
};

struct cbDepthPyramid
{   
    uint4_(Mips0_3);
    uint4_(Mips4_7);
    uint4_(Mips8_11);

    uint16_t MipLevels;
    uint16_t NumThreadGroupsX;
    uint16_t NumThreadGroupsY;
    uint16_t Mip5DimX;
    uint16_t Mip5DimY;
    uint16_t pad;
};

struct cbGBuffer
{
    uint32_t MeshIdxinBuff;
};

struct cbOcclussionCulling
{
    float4x4_ ViewProj;
    uint32_t NumMeshes;
    uint32_t CounterBufferOffset;
    uint32_t MeshBufferStartIndex;
    uint32_t ArgBufferStartOffsetInBytes;
    uint32_t DepthPyramidSrvDescHeapIdx;
    float DepthThresh;
    uint16_t DepthPyramidMip0DimX;
    uint16_t DepthPyramidMip0DimY;
    uint16_t NumDepthPyramidMips;
    uint16_t pad;
};

#endif