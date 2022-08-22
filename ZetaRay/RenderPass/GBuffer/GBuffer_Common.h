#ifndef GBUFFER_H
#define GBUFFER_H

#include "../Common/HLSLCompat.h"

struct DrawCB
{
	row_major float3x4_ CurrWorld;
	row_major float4x4_ CurrWorldInvT;
	row_major float3x4_ PrevWorld;
	row_major float4x4_ PrevWorldInvT;
	uint_ MatID;
	uint_ pad[3];
};

#endif