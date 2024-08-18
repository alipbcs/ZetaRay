#ifndef HLSL_COMPAT
#define HLSL_COMPAT

#ifdef __cplusplus
    #include <cstdint>
    #include "../Math/Matrix.h"

    #define float2_ ZetaRay::Math::float2
    #define float3_ ZetaRay::Math::float3
    #define float4_ ZetaRay::Math::float4
    #define float3x3_ ZetaRay::Math::float3x3
    #define float3x4_ ZetaRay::Math::float3x4
    #define float4x4_ ZetaRay::Math::float4x4a
    #define half_ ZetaRay::Math::half
    #define half2_ ZetaRay::Math::half2
    #define half3_ ZetaRay::Math::half3
    #define half4_ ZetaRay::Math::half4
    #define unorm2_ ZetaRay::Math::unorm2
    #define unorm3_ ZetaRay::Math::unorm3
    #define unorm4_ ZetaRay::Math::unorm4

    #define IN_PARAM(t) t&
    #define OUT_PARAM(t) t&
    #define row_major
    #define CONST const

    #define IS_CB_FLAG_SET(cb, flag) ((cb.Flags & (flag)) == (flag))
    #define SET_CB_FLAG(cb, flag, val) (cb.Flags = (cb.Flags & ~(flag)) | ((val) * (flag)))
#else
    #define float2_ float2
    #define float3_ float3
    #define float4_ float4
    #define float3x3_ float3x3
    #define float3x3_ float3x3
    #define float3x4_ float3x4
    #define float4x3_ float4x3
    #define float4x4_ float4x4
    #define half_ half
    #define half2_ half2
    #define half3_ half3
    #define half4_ half4
    #define unorm2_ uint16_t2
    #define unorm3_ uint16_t3
    #define unorm4_ uint16_t4
    #define uint16 uint16_t
    #define uint32 uint32_t
    #define int16 int16_t
    #define int32 int32_t

    #define IN_PARAM(t) in t
    #define OUT_PARAM(t) out t
    #define CONST
    #define constexpr const

    #define IS_CB_FLAG_SET(flag) ((g_local.Flags & flag) == flag)
#endif

#endif // HLSL_COMPAT