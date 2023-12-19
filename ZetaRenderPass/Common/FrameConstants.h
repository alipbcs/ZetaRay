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

		float3_ CameraPos;
		float CameraNear;

		float AspectRatio;
		float PixelSpreadAngle;
		float TanHalfFOV;
		float dt;

		uint32_t FrameNum;
		uint32_t MetallicRoughnessMapsDescHeapOffset;
		uint32_t EmissiveMapsDescHeapOffset;
		uint32_t EnvMapDescHeapOffset;

		float2_ CurrProjectionJitter;
		float2_ PrevProjectionJitter;

		uint32_t CurrGBufferDescHeapOffset;
		uint32_t PrevGBufferDescHeapOffset;
		uint32_t BaseColorMapsDescHeapOffset;
		uint32_t NormalMapsDescHeapOffset;

		uint32_t RenderWidth;
		uint32_t RenderHeight;
		uint32_t DisplayWidth;
		uint32_t DisplayHeight;

		float PlanetRadius;
		float SunCosAngularRadius;
		float SunSinAngularRadius;
		float MipBias;

		float3_ SunDir;
		float SunIlluminance;

		float3_ RayleighSigmaSColor;
		float RayleighSigmaSScale;

		float3_ OzoneSigmaAColor;
		float OzoneSigmaAScale;

		float MieSigmaS;
		float MieSigmaA;
		float AtmosphereAltitude;
		float g;

		uint32_t NumFramesCameraStatic;
		uint32_t CameraStatic;
		uint32_t Accumulate;
		uint32_t pad2;
	};
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert((offsetof(ZetaRay::cbFrameConstants, CameraPos) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, AspectRatio) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, FrameNum) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, CurrProjectionJitter) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, CurrGBufferDescHeapOffset) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, RenderWidth) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, PlanetRadius) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, SunDir) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, RayleighSigmaSColor) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, OzoneSigmaAColor) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, MieSigmaS) & 15) == 0);
#endif

#endif
