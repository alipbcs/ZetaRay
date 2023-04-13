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
	#define	uint4_(x) uint32_t x[4]
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
	#define uint4_(x) uint4 x
#endif

#ifdef __cplusplus
#define IN_PARAM(t) t&
#define OUT_PARAM(t) t&
#define CONST const
#else
#define IN_PARAM(t) in t
#define OUT_PARAM(t) out t
#define CONST
#endif // __cplusplus

#ifdef __cplusplus
#ifndef row_major
#define row_major
#endif
#endif

#endif // HLSL_COMPAT