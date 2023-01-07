#ifndef SAMPLERS_H
#define SAMPLERS_H

SamplerState g_samPointWrap : register(s0);
SamplerState g_samPointClamp : register(s1);
SamplerState g_samLinearWrap : register(s2);
SamplerState g_samLinearClamp : register(s3);
SamplerState g_samAnisotropicWrap : register(s4);
SamplerState g_samAnisotropicClamp : register(s5);
SamplerState g_samImgUi : register(s6);
SamplerState g_samMinPointClamp : register(s7);

#endif