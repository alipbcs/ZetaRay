#ifndef GBUFFER_H
#define GBUFFER_H

#include "../Common/HLSLCompat.h"

struct DrawCB
{
	row_major float3x4_ CurrWorld;
	row_major float3x4_ PrevWorld;
	uint32_t MatID;
	uint32_t pad[3];
};

#endif