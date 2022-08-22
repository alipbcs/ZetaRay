#ifndef INDIRECT_DIFFUSE_H
#define INDIRECT_DIFFUSE_H

#include "../Common/HLSLCompat.h"

#define RT_IND_DIFF_THREAD_GROUP_SIZE_X 8
#define RT_IND_DIFF_THREAD_GROUP_SIZE_Y 8
#define RT_IND_DIFF_THREAD_GROUP_SIZE_Z 1

struct cbIndirectDiffuse
{
	uint_ OutputDescHeapIdx;
	uint16_t InputWidth;
	uint16_t InputHeight;
	uint16_t DispatchDimX;
	uint16_t DispatchDimY;
	uint16_t TileWidth;
	uint16_t Log2TileWidth;
	uint16_t NumGroupsInTile;
};

#endif