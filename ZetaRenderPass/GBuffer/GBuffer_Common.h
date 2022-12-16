#ifndef GBUFFER_H
#define GBUFFER_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define OCCLUSION_CULL_THREAD_GROUP_SIZE_X 32
#define OCCLUSION_CULL_THREAD_GROUP_SIZE_Y 1
#define OCCLUSION_CULL_THREAD_GROUP_SIZE_Z 1

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

    uint32_t pad0;
    uint32_t pad1;
};

struct MeshInstance
{
    row_major float3x4_ CurrWorld;
    row_major float3x4_ PrevWorld;
    uint32_t IndexCount;
    uint32_t BaseVtxOffset;
    uint32_t BaseIdxOffset;
    uint16_t IdxInMatBuff;
    uint16_t pad0;
};

struct cbGBuffer
{
    uint32_t MeshIdxinBuff;
};

struct cbOcclussionCulling
{
    uint32_t NumMeshes;
    uint32_t CounterBufferOffset;
};

#endif