#ifndef SUN_SHADOW_COMMON_H
#define SUN_SHADOW_COMMON_H

#include "../Common/HLSLCompat.h"

#define SUN_SHADOW_THREAD_GROUP_SIZE_X 8
#define SUN_SHADOW_THREAD_GROUP_SIZE_Y 4
#define SUN_SHADOW_THREAD_GROUP_SIZE_Z 1

struct cbSunShadow
{
	uint32_t OutShadowMaskDescHeapIdx;
};

#endif
