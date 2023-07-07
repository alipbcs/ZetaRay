#ifndef RTCOMMON_H
#define RTCOMMON_H

#include "../Core/HLSLCompat.h"

#ifdef __cplusplus
#include "../Math/VectorFuncs.h"
#endif

// Meshes present in an Acceleration Structure, can be subdivided into groups
// based on a specified 8-bit mask value. During ray travesal, instance mask from 
// the ray and corresponding mask from each mesh are ANDed together. Mesh is skipped
// if the result is zero.
namespace RT_AS_SUBGROUP
{
	static const uint32_t EMISSIVE = 0x1;
	static const uint32_t NON_EMISSIVE = 0x2;
	static const uint32_t ALL = EMISSIVE | NON_EMISSIVE;
}

#ifdef __cplusplus
namespace ZetaRay
{
#endif
	namespace RT
	{
		struct MeshInstance
		{
			uint32_t BaseVtxOffset;
			uint32_t BaseIdxOffset;
			half4_ Rotation;
			half3_ Scale;
			uint16_t MatID;
		};

		struct EmissiveTriangle
		{
			static const uint32_t V0V1SignBit = 24;
			static const uint32_t V0V2SignBit = 25;

#ifdef __cplusplus
			EmissiveTriangle() = default;
			EmissiveTriangle(const Math::float3& vtx0, const Math::float3& vtx1, const Math::float3& vtx2,
				const Math::float2& uv0, const Math::float2& uv1, const Math::float2& uv2,
				uint32_t emissiveFactor, uint32_t EmissiveTex_Strength) noexcept
				: Vtx0(vtx0),
				EmissiveFactor_Signs(emissiveFactor & 0xffffff),
				EmissiveTex_Strength(EmissiveTex_Strength),
				UV0(uv0),
				UV1(uv1),
				UV2(uv2)
			{
				__m128 v0 = Math::loadFloat3(const_cast<Math::float3&>(vtx0));
				__m128 v1 = Math::loadFloat3(const_cast<Math::float3&>(vtx1));
				__m128 v2 = Math::loadFloat3(const_cast<Math::float3&>(vtx2));

				StoreVertices(v0, v1, v2);
			}
#endif

			uint32_t EmissiveFactor_Signs;
			uint32_t EmissiveTex_Strength;

			float3_ Vtx0;
			float2_ UV0;
			float2_ UV1;
			float2_ UV2;

			half3_ V0V1;
			half3_ V0V2;

#ifdef __cplusplus
			ZetaInline void __vectorcall StoreVertices(__m128 v0, __m128 v1, __m128 v2) noexcept
			{
				Vtx0 = Math::storeFloat3(v0);

				__m128 v0v1 = _mm_sub_ps(v1, v0);
				__m128 v0v2 = _mm_sub_ps(v2, v0);

				__m128 v0v1Len = Math::length(v0v1);
				__m128 v0v2Len = Math::length(v0v2);

				__m128 tmp1 = _mm_div_ps(v0v1, v0v1Len);
				__m128 tmp2 = _mm_div_ps(v0v2, v0v2Len);

				// set z component to length
				tmp1 = _mm_insert_ps(tmp1, v0v1Len, 0x20);
				tmp2 = _mm_insert_ps(tmp2, v0v2Len, 0x20);

				// store v0v1
 				__m128i vH = _mm_cvtps_ph(tmp1, 0);
				vH = _mm_unpacklo_epi16(vH, _mm_castps_si128(_mm_setzero_ps()));

				V0V1.x = static_cast<uint16_t>(_mm_cvtsi128_si32(vH));
				V0V1.y = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x1)));
				V0V1.z = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x2)));

				// store v0v2
				vH = _mm_cvtps_ph(tmp2, 0);
				vH = _mm_unpacklo_epi16(vH, _mm_castps_si128(_mm_setzero_ps()));

				V0V2.x = static_cast<uint16_t>(_mm_cvtsi128_si32(vH));
				V0V2.y = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x1)));
				V0V2.z = static_cast<uint16_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(vH, 0x2)));

				// remember sign of z component
				__m128 vZero = _mm_setzero_ps();
				int cmpMask0 = _mm_movemask_ps(_mm_cmpge_ps(v0v1, vZero));
				int isPos0 = (cmpMask0 & 0x4) == 0x4;

				int cmpMask1 = _mm_movemask_ps(_mm_cmpge_ps(v0v2, vZero));
				int isPos1 = (cmpMask1 & 0x4) == 0x4;

				// clear the sign bits
				EmissiveFactor_Signs &= ~((1u << V0V1SignBit) | (1u << V0V2SignBit));

				EmissiveFactor_Signs = EmissiveFactor_Signs | (isPos0 << V0V1SignBit);
				EmissiveFactor_Signs = EmissiveFactor_Signs | (isPos1 << V0V2SignBit);
			}

			ZetaInline void __vectorcall LoadVertices(__m128& v0, __m128& v1, __m128& v2) noexcept
			{
				bool signIsPosV1 = EmissiveFactor_Signs & (1u << V0V1SignBit);
				bool signIsPosV2 = EmissiveFactor_Signs & (1u << V0V2SignBit);
				__m128 V1Mask = _mm_setr_ps(1.0f, 1.0f, signIsPosV1 ? 1.0f : -1.0f, 1.0f);
				__m128 V2Mask = _mm_setr_ps(1.0f, 1.0f, signIsPosV2 ? 1.0f : -1.0f, 1.0f);

				// convert from half to float
				uint64_t packedV0V1 = V0V1.x | (uint32_t(V0V1.y) << 16) | (uint64_t(V0V1.z) << 32);
				uint64_t packedV0V2 = V0V2.x | (uint32_t(V0V2.y) << 16) | (uint64_t(V0V2.z) << 32);
				__m128i vPackedV0V1 = _mm_cvtsi64_si128(packedV0V1);
				__m128i vPackedV0V2 = _mm_cvtsi64_si128(packedV0V2);	
				__m128 vV0V1_len = _mm_cvtph_ps(vPackedV0V1);
				__m128 vV0V2_len = _mm_cvtph_ps(vPackedV0V2);
				// extract length (z component)
				__m128 vLenV0V1 = _mm_shuffle_ps(vV0V1_len, vV0V1_len, V_SHUFFLE_XYZW(2, 2, 2, 0));
				__m128 vLenV0V2 = _mm_shuffle_ps(vV0V2_len, vV0V2_len, V_SHUFFLE_XYZW(2, 2, 2, 0));

				__m128 vOne = _mm_set1_ps(1.0f);
				__m128 vVtx0 = Math::loadFloat3(Vtx0);
				// set v[3] = 1
				vVtx0 = _mm_insert_ps(vVtx0, vOne, 0x30);
				v0 = vVtx0;
				// z = sqrt(1 - x * x - y * y)
				__m128 vTemp = _mm_shuffle_ps(vV0V1_len, vV0V2_len, V_SHUFFLE_XYZW(0, 1, 0, 1));
				vTemp = _mm_mul_ps(vTemp, vTemp);
				vTemp = _mm_hadd_ps(vTemp, vTemp);
				vTemp = _mm_sub_ps(vOne, vTemp);
				// due to conversion to half, result could be negative, which leads to NaN in sqrt
				vTemp = _mm_max_ps(vTemp, _mm_setzero_ps());
				vTemp = _mm_sqrt_ps(vTemp);

				// restore the z component
				__m128 vV1 = _mm_insert_ps(vV0V1_len, vTemp, 0x20);
				// and restore its sign
				vV1 = _mm_mul_ps(vV1, V1Mask);
				// interpolate
				vV1 = _mm_fmadd_ps(vV1, vLenV0V1, vVtx0);
				v1 = vV1;

				__m128 vV2 = _mm_insert_ps(vV0V2_len, vTemp, 0x60);
				vV2 = _mm_mul_ps(vV2, V2Mask);
				vV2 = _mm_fmadd_ps(vV2, vLenV0V2, vVtx0);
				v2 = vV2;
			}
#endif

#ifndef __cplusplus
			float3 V1()
			{
				float sign = (EmissiveFactor_Signs & (1u << V0V1SignBit)) ? 1.0f : -1.0f;
				float3 v0v1 = float3(V0V1.x, V0V1.y, 0.0f);
				float z2 = 1 - v0v1.x * v0v1.x - v0v1.y * v0v1.y;
				z2 = max(0, z2);
				v0v1.z = sqrt(z2);
				v0v1.z *= sign;
				return mad(v0v1, V0V1.z, Vtx0);
			}

			float3 V2()
			{
				float sign = (EmissiveFactor_Signs & (1u << V0V2SignBit)) ? 1.0f : -1.0f;
				float3 v0v2 = float3(V0V2.x, V0V2.y, 0.0f);
				float z2 = 1 - v0v2.x * v0v2.x - v0v2.y * v0v2.y;
				z2 = max(0, z2);
				v0v2.z = sqrt(z2);
				v0v2.z *= sign;
				return mad(v0v2, V0V2.z, Vtx0);
			}

			uint16_t GetEmissiveTex()
			{
				return uint16_t(EmissiveTex_Strength & 0xffff);
			}

			half GetEmissiveStrength()
			{
				return asfloat16(uint16_t(EmissiveTex_Strength >> 16));
			}
#endif
		};
	}
#ifdef __cplusplus
}
#endif

#endif
