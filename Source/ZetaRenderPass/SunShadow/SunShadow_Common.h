#ifndef SUN_SHADOW_COMMON_H
#define SUN_SHADOW_COMMON_H

#include "../../ZetaCore/Core/HLSLCompat.h"

#define SUN_SHADOW_THREAD_GROUP_SIZE_X 8u
#define SUN_SHADOW_THREAD_GROUP_SIZE_Y 8u

#define DNSR_TEMPORAL_THREAD_GROUP_SIZE_X 8u
#define DNSR_TEMPORAL_THREAD_GROUP_SIZE_Y 8u

#define DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_X 8u
#define DNSR_SPATIAL_FILTER_THREAD_GROUP_SIZE_Y 8u

struct cbSunShadow
{
    uint32_t OutShadowMaskDescHeapIdx;
    uint32_t SoftShadows;
};

struct cbFFX_DNSR_Temporal
{
    uint32_t ShadowMaskSRVDescHeapIdx;
    uint32_t MetadataUAVDescHeapIdx;
    uint32_t MomentsUAVHeapIdx;
    uint32_t PrevTemporalDescHeapIdx;
    uint32_t CurrTemporalDescHeapIdx;
    uint32_t DenoisedDescHeapIdx;
    uint16_t NumShadowMaskThreadGroupsX;
    uint16_t NumShadowMaskThreadGroupsY;

    uint16_t IsTemporalValid;
    uint16_t Denoise;
};

struct cbFFX_DNSR_Spatial
{
    uint32_t MetadataSRVDescHeapIdx;
    uint32_t InTemporalDescHeapIdx;
    uint32_t OutTemporalDescHeapIdx;
    uint32_t DenoisedDescHeapIdx;
    float EdgeStoppingNormalExp;
    float EdgeStoppingShadowStdScale;
    float MinFilterVar;
    uint32_t PassNum;
    uint32_t StepSize;
    uint32_t WriteDenoised;
};

#endif
