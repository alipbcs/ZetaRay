#ifndef COMPOSITING_H
#define COMPOSITING_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define COMPOSITING_THREAD_GROUP_DIM_X 8u
#define COMPOSITING_THREAD_GROUP_DIM_Y 8u

#define FIREFLY_FILTER_THREAD_GROUP_DIM_X 16u
#define FIREFLY_FILTER_THREAD_GROUP_DIM_Y 8u

namespace CB_COMPOSIT_FLAGS
{
    static constexpr uint32_t SKY_DI = 1 << 1;
    static constexpr uint32_t INDIRECT = 1 << 2;
    static constexpr uint32_t INSCATTERING = 1 << 4;
    static constexpr uint32_t EMISSIVE_DI = 1 << 5;
    static constexpr uint32_t VISUALIZE_LVG = 1 << 6;
};

struct cbCompositing
{
    uint32_t InscatteringDescHeapIdx;
    uint32_t SkyDIDescHeapIdx;
    uint32_t EmissiveDIDescHeapIdx;
    uint32_t IndirectDescHeapIdx;
    uint32_t OutputUAVDescHeapIdx;

    float DepthMappingExp;
    float VoxelGridNearZ;
    float VoxelGridFarZ;
    float Offset_y;

    uint32_t Flags;

    float Extents_x;
    float Extents_y;
    float Extents_z;
    uint16_t GridDim_x;
    uint16_t GridDim_y;
    uint16_t GridDim_z;
    uint16_t pad;
};

struct cbFireflyFilter
{
    uint32_t CompositedUAVDescHeapIdx;
};

#endif