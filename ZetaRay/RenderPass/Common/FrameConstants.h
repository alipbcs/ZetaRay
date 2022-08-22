#ifndef FRAME_CONSTANTS
#define FRAME_CONSTANTS

#include "HLSLCompat.h"

#ifdef __cplusplus
namespace ZetaRay
{
#endif

	struct cbFrameConstants
	{
		row_major float4x4_ CurrProj;
		row_major float3x4_ CurrView;
		row_major float4x4_ CurrViewProj;
		row_major float4x4_ PrevViewProj;
		row_major float3x4_ CurrViewInv;
		row_major float3x4_ PrevViewInv;
		//
		// camera
		//
		float3_ CameraPos;
		float CameraNear;

		float CameraFar;
		float AspectRatio;
		float PixelSpreadAngle;
		float TanHalfFOV;

		uint_ RenderWidth;
		uint_ RenderHeight;
		uint_ DisplayWidth;
		uint_ DisplayHeight;

		float2_ CurrCameraJitter;
		float2_ PrevCameraJitter;
		//
		// GBuffer
		//
		uint_ CurrGBufferDescHeapOffset;
		uint_ PrevGBufferDescHeapOffset;
		float MipBias;
		uint_ pad1;
		//
		// Material
		//
		uint_ BaseColorMapsDescHeapOffset;
		uint_ NormalMapsDescHeapOffset;
		uint_ EmissiveMapsDescHeapOffset;
		uint_ MetalnessRoughnessMapsDescHeapOffset;
		//
		// SkyDome
		// 
		uint_ FrameNum;
		uint_ EnvMapDescHeapOffset;
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

		float MieSigmaS;		// Mie-scattering is not wavelength dependant
		float MieSigmaA;
		float AtmosphereAltitude;
		float g;

		float PlanetRadius;
		float3_ pad3;
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
