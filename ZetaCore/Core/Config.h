#pragma once

// PIX crashes & NSight doesn't work when the debug layer is enabled
#define DIREC3D_DEBUG_LAYER 1

// To ensure stable GPU frequency for performance testing. Requires developer mode to be enabled.
#define STABLE_GPU_POWER_STATE 1

// Raytraced G-Buffer
#define RT_GBUFFER 1
