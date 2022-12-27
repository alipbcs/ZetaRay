#ifndef HLSL_COMPAT
#define HLSL_COMPAT

#ifdef __cplusplus
	#include <cstdint>
	#include "../Math/Matrix.h"
	
	#define half_ uint16_t
	#define half2(x) uint16_t x[2]
	#define half3(x) uint16_t x[3]
	#define half4(x) uint16_t x[4]
	#define float2_ ZetaRay::Math::float2
	#define float3_ ZetaRay::Math::float3
	#define float4_ ZetaRay::Math::float4
	#define float3x3_ ZetaRay::Math::float3x3
	#define float3x4_ ZetaRay::Math::float3x4
	#define float4x4_ ZetaRay::Math::float4x4a
#else
	#define half_ half
	#define half2_ half2
	#define half3_ half3
	#define half4_ half4
	#define float2_ float2
	#define float3_ float3
	#define float4_ float4
	#define float3x3_ float3x3
	#define float3x3_ float3x3
	#define float3x4_ float3x4
	#define float4x3_ float4x3
	#define float4x4_ float4x4
	#define index_type INDEX_TYPE
#endif

#define INDEX_TYPE uint32_t

#ifdef __cplusplus
#define IN_PARAM(t) t&
#define OUT_PARAM(t) t&
#define CONST const
#else
#define IN_PARAM(t) in t
#define OUT_PARAM(t) out t
#define CONST
#endif // __cplusplus

//#ifndef __cplusplus
//static const uint INVALID_INDEX = uint(-1);
//#endif

#ifdef __cplusplus
#ifndef row_major
#define row_major
#endif
#endif

#endif // HLSL_COMPAT