#ifndef GBUFFERS_H
#define GBUFFERS_H

#include "../../ZetaCore/Core/Material.h"

enum GBUFFER_OFFSET
{
	BASE_COLOR = 0,
    NORMAL,
    METALLIC_ROUGHNESS,
    MOTION_VECTOR,
	EMISSIVE_COLOR,
	CURVATURE,
    DEPTH
};

#define GBUFFER_BASE_COLOR Texture2D<half4>
#define GBUFFER_NORMAL Texture2D<half2>
#define GBUFFER_METALLIC_ROUGHNESS Texture2D<half2>
#define GBUFFER_MOTION_VECTOR Texture2D<half2> 
#define GBUFFER_EMISSIVE_COLOR Texture2D<half4>
#define GBUFFER_DEPTH Texture2D<float> 
#define GBUFFER_CURVATURE Texture2D<float> 

namespace GBuffer
{
	float EncodeMetallic(float metalness, uint baseColorTexture, float3 emissive)
	{
		bool isMetal = metalness >= MIN_METALNESS_METAL;
		bool isEmissive = dot(1, emissive) != 0;
	
		uint ret = baseColorTexture != -1;
		ret |= isEmissive << 1;
		ret |= isMetal << 7;

		return float(ret) / 255.0f;
	}
	
	void DecodeMetallic(float encoded, out bool isMetallic, out bool hasBaseColorTexture, out bool isEmissive)
	{
		uint v = (uint) round(encoded * 255.0f);

		hasBaseColorTexture = (v & 0x1) != 0;
		isEmissive = (v & (1 << 1)) != 0;
		isMetallic = (v & (1 << 7)) != 0;
	}
	
	bool4 DecodeMetallic(float4 encoded)
	{
		uint4 v = (uint4) round(encoded * 255.0f);
		return (v & (1 << 7)) != 0;
	}
	
	void DecodeMetallicEmissive(float4 encoded, out bool4 isMetallic, out bool4 isEmissive)
	{
		uint4 v = (uint4) round(encoded * 255.0f);
		isEmissive = (v & (1 << 1)) != 0;
		isMetallic = (v & (1 << 7)) != 0;
	}
	
	bool DecodeMetallic(float encoded)
	{
		uint v = (uint) round(encoded * 255.0f);
		return (v & (1 << 7)) != 0;
	}
	
	bool DecodeHasBaseColorTexture(float encoded)
	{
		uint v = (uint) round(encoded * 255.0f);
		return (v & 0x1) != 0;
	}
	
	bool DecodeIsEmissive(float encoded)
	{
		uint v = (uint) round(encoded * 255.0f);
		return (v & (1 << 1)) != 0;
	}
	
	void DecodeMetallicEmissive(float encoded, out bool isMetallic, out bool isEmissive)
	{
		uint v = (uint) round(encoded * 255.0f);
		isEmissive = (v & (1 << 1)) != 0;
		isMetallic = (v & (1 << 7)) != 0;
	}
}

#endif