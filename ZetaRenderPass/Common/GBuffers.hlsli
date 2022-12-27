#ifndef GBUFFERS_H
#define GBUFFERS_H

enum GBUFFER_OFFSET
{
	BASE_COLOR = 0,
    NORMAL,
    METALLIC_ROUGHNESS,
    MOTION_VECTOR,
	EMISSIVE_COLOR,
    DEPTH
};

#define GBUFFER_BASE_COLOR Texture2D<half4>
#define GBUFFER_NORMAL Texture2D<half2>
#define GBUFFER_METALLIC_ROUGHNESS Texture2D<half2>
#define GBUFFER_MOTION_VECTOR Texture2D<half2> 
#define GBUFFER_EMISSIVE_COLOR Texture2D<half4>
#define GBUFFER_DEPTH Texture2D<float> 

#endif