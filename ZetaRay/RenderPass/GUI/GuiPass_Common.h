#ifndef GUI_PASS_H
#define GUI_PASS_H

#include "../Common/HLSLCompat.h"

struct cbGuiPass
{
    float4x4_ WVP;

	// Texture2D<float4>
    uint32_t FontTex;
    uint32_t pad[3];
};

#endif 
