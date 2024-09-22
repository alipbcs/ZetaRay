#ifndef FRAME_CONSTANTS
#define FRAME_CONSTANTS

#include "../../ZetaCore/Core/HLSLCompat.h"

#ifdef __cplusplus
namespace ZetaRay
{
#endif
    struct cbFrameConstants
    {
        row_major float3x4_ CurrView;
        row_major float3x4_ PrevView;
        row_major float3x4_ CurrViewInv;
        row_major float3x4_ PrevViewInv;
        float4x4_ CurrViewProj;
        float4x4_ PrevViewProj;

        float3_ CameraPos;
        float CameraNear;

        float AspectRatio;
        float PixelSpreadAngle;
        float TanHalfFOV;
        float dt;

        uint32_t FrameNum;
        uint32_t CurrGBufferDescHeapOffset;
        uint32_t PrevGBufferDescHeapOffset;
        uint32_t BaseColorMapsDescHeapOffset;

        uint32_t NormalMapsDescHeapOffset;
        uint32_t MetallicRoughnessMapsDescHeapOffset;
        uint32_t EmissiveMapsDescHeapOffset;
        uint32_t EnvMapDescHeapOffset;

        uint32_t RenderWidth;
        uint32_t RenderHeight;
        uint32_t DisplayWidth;
        uint32_t DisplayHeight;

        float2_ CurrCameraJitter;
        float2_ PrevCameraJitter;

        float PlanetRadius;
        float SunCosAngularRadius;
        float SunSinAngularRadius;
        float pad;

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
        uint32_t SunMoved;

        float CameraRayUVGradsScale;
        float MipBias;
        float OneDivNumEmissiveTriangles;
        uint32_t NumEmissiveTriangles;

        float FocusDepth;
        float LensRadius;
        uint32_t DoF;
        uint32_t pad2;
    };
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert((offsetof(ZetaRay::cbFrameConstants, CameraPos) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, AspectRatio) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, FrameNum) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, NormalMapsDescHeapOffset) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, RenderWidth) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, CurrCameraJitter) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, PlanetRadius) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, SunDir) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, RayleighSigmaSColor) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, OzoneSigmaAColor) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, MieSigmaS) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, NumFramesCameraStatic) & 15) == 0);
static_assert((offsetof(ZetaRay::cbFrameConstants, CameraRayUVGradsScale) & 15) == 0);
#endif

#endif
