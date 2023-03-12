#ifndef FRAME_CONSTANTS
#define FRAME_CONSTANTS

#include "../../ZetaCore/Core/HLSLCompat.h"

#ifdef __cplusplus
namespace ZetaRay
{
#endif

	struct cbFrameConstants
	{
		row_major float4x4_ CurrProj;
		row_major float3x4_ CurrView;
		row_major float3x4_ PrevView;
		float4x4_ CurrViewProj;
		float4x4_ PrevViewProj;
		row_major float3x4_ CurrViewInv;
		row_major float3x4_ PrevViewInv;
		//
		// camera
		//
		float3_ CameraPos;
		float CameraNear;

		float AspectRatio;
		float PixelSpreadAngle;
		float TanHalfFOV;
		float PlanetRadius;

		uint32_t RenderWidth;
		uint32_t RenderHeight;
		uint32_t DisplayWidth;
		uint32_t DisplayHeight;

		float2_ CurrCameraJitter;
		float2_ PrevCameraJitter;
		//
		// GBuffer
		//
		uint32_t CurrGBufferDescHeapOffset;
		uint32_t PrevGBufferDescHeapOffset;
		float MipBias;
		float dt;
		//
		// Material
		//
		uint32_t BaseColorMapsDescHeapOffset;
		uint32_t NormalMapsDescHeapOffset;
		uint32_t MetalnessRoughnessMapsDescHeapOffset;
		uint32_t EmissiveMapsDescHeapOffset;
		//
		// SkyDome
		// 
		uint32_t FrameNum;
		uint32_t EnvMapDescHeapOffset;
		float WorldRadius;
		float RayOffset;
		//
		// Sun
		//
		float3_ SunDir;
		float SunIlluminance;
		float SunCosAngularRadius;
		float3_ pad2;
		//
		// Atmosphere
		//
		float3_ RayleighSigmaSColor;
		float RayleighSigmaSScale;
		
		float3_ OzoneSigmaAColor;
		float OzoneSigmaAScale;

		float MieSigmaS;		// Mie-scattering is not wavelength dependent
		float MieSigmaA;
		float AtmosphereAltitude;
		float g;
	};

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert((offsetof(ZetaRay::cbFrameConstants, CameraPos) % 16) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, CurrCameraJitter) % 16) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, SunDir) % 16) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, RayleighSigmaSColor) % 16) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, OzoneSigmaAColor) % 16) == 0);
#endif

#endif
