#ifndef HLSL_COMPAT
#define HLSL_COMPAT

#ifdef __cplusplus
	#include <cstdint>
	#include "../../Math/Matrix.h"
	
	#define uint_ uint32_t
	#define half_ uint16_t
	#define float2_ ZetaRay::Math::float2
	#define float3_ ZetaRay::Math::float3
	#define float4_ ZetaRay::Math::float4
	#define float3x3_ ZetaRay::Math::float3x3
	#define float3x4_ ZetaRay::Math::float3x4
	#define float4x4_ ZetaRay::Math::float4x4a
#else
	#define uint_ uint
	#define half_ half
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

#define USE_16_BIT_INDICES 0

#if USE_16_BIT_INDICES
#define INDEX_TYPE uint16_t
#else
#define INDEX_TYPE uint32_t
#endif

#ifdef __cplusplus
#define IN_PARAM(t) t&
#define OUT_PARAM(t) t&
#else
#define IN_PARAM(t) in t
#define OUT_PARAM(t) out t
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