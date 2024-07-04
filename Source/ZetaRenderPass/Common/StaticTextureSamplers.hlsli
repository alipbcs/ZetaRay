#ifndef SAMPLERS_H
#define SAMPLERS_H

SamplerState g_samMip0 : register(s0);
SamplerState g_samPointWrap : register(s1);
SamplerState g_samPointClamp : register(s2);
SamplerState g_samLinearWrap : register(s3);
SamplerState g_samLinearClamp : register(s4);
SamplerState g_samAnisotropicWrap : register(s5);
SamplerState g_samAnisotropicWrap_4x : register(s6);
SamplerState g_samImgUi : register(s7);

#endif