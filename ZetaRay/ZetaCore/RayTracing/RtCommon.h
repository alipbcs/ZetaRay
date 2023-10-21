#ifndef RTCOMMON_H
#define RTCOMMON_H

#include "../Core/HLSLCompat.h"

#ifdef __cplusplus
#include "../Math/VectorFuncs.h"
#endif

#define ENCODE_EMISSIVE_POS 1

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
			snorm4_ Rotation;
			half3_ Scale;
			uint16_t MatIdx;
			uint32_t BaseEmissiveTriOffset;

			float3_ Translation;
			snorm4_ PrevRotation;
			half3_ PrevScale;
			half3_ dTranslation;

			// inline alpha stuff to avoid loading material data in anyhit shaders
			uint16_t BaseColorTex;
			uint16_t AlphaFactor_Cuttoff;
		};

		struct EmissiveTriangle
		{
			static const uint32_t V0V1SignBit = 24;
			static const uint32_t V0V2SignBit = 25;
			static const uint32_t TriIDPatchedBit = 26;
			static const uint32_t DoubleSidedBit = 27;

#ifdef __cplusplus
			EmissiveTriangle() = default;
			EmissiveTriangle(const Math::float3& vtx0, const Math::float3& vtx1, const Math::float3& vtx2,
				const Math::float2& uv0, const Math::float2& uv1, const Math::float2& uv2,
				uint32_t emissiveFactor, uint32_t emissiveTex_Strength, uint32_t triIdx, bool doubleSided = true)
				: ID(triIdx),
				EmissiveFactor_Signs(emissiveFactor & 0xffffff),
				EmissiveTex_Strength(emissiveTex_Strength),
				UV0(uv0),
				UV1(uv1),
				UV2(uv2)
			{
				__m128 v0 = Math::loadFloat3(const_cast<Math::float3&>(vtx0));
				__m128 v1 = Math::loadFloat3(const_cast<Math::float3&>(vtx1));
				__m128 v2 = Math::loadFloat3(const_cast<Math::float3&>(vtx2));
				StoreVertices(v0, v1, v2);

				EmissiveFactor_Signs |= (doubleSided << DoubleSidedBit);
			}
#endif

			float3_ Vtx0;

#if ENCODE_EMISSIVE_POS == 1
			snorm2_ V0V1;
			snorm2_ V0V2;
			half2_ EdgeLengths;
#else
			float3_ Vtx1;
			float3_ Vtx2;
#endif

			uint32_t ID;
			uint32_t EmissiveFactor_Signs;

			float2_ UV0;
			float2_ UV1;
			float2_ UV2;
			uint32_t EmissiveTex_Strength;

#ifdef __cplusplus
			ZetaInline void __vectorcall StoreVertices(__m128 v0, __m128 v1, __m128 v2)
			{
				Vtx0 = Math::storeFloat3(v0);

#if ENCODE_EMISSIVE_POS == 1
				__m256 vV0V0 = _mm256_insertf128_ps(_mm256_castps128_ps256(v0), v0, 1);
				__m256 vV1V2 = _mm256_insertf128_ps(_mm256_castps128_ps256(v1), v2, 1);

				// (v1 - v0, v2 - v0)
				__m256 vE0E1 = _mm256_sub_ps(vV1V2, vV0V0);

				// (||v1 - v0||, ||v2 - v0||, _, _)
				__m256 vE0E1_2 = _mm256_mul_ps(vE0E1, vE0E1);
				__m128 vLower = _mm256_extractf128_ps(vE0E1_2, 0);
				__m128 vUpper = _mm256_extractf128_ps(vE0E1_2, 1);
				__m128 vEdgeLengths = _mm_hadd_ps(vLower, vUpper);
				vEdgeLengths = _mm_hadd_ps(vEdgeLengths, vEdgeLengths);
				vEdgeLengths = _mm_sqrt_ps(vEdgeLengths);

				// (||e0||, ||e0||, ||e0||, ||e0||, ||e1||, ||e1||, ||e1||, ||e1||)
				__m256 vEdgeLengthsSplatted = _mm256_insertf128_ps(
					_mm256_castps128_ps256(_mm_shuffle_ps(vEdgeLengths, vEdgeLengths, V_SHUFFLE_XYZW(0, 0, 0, 0))),
					_mm_shuffle_ps(vEdgeLengths, vEdgeLengths, V_SHUFFLE_XYZW(1, 1, 1, 1)), 1);
				// (v1 - v0, v2 - v0)
				__m256 vE0E1Normalized = _mm256_div_ps(vE0E1, vEdgeLengthsSplatted);

				// encode using 16-bit SNORMs
				__m256 vMax = _mm256_set1_ps((1 << 15) - 1);
				__m256 vTemp = _mm256_mul_ps(vE0E1Normalized, vMax);
				vTemp = _mm256_round_ps(vTemp, 0);
				__m256i vE0E1Encoded = _mm256_cvtps_epi32(vTemp);

				// store normalized edges
				__m128i vEdge0 = _mm_castps_si128(_mm256_extractf128_ps(_mm256_castsi256_ps(vE0E1Encoded), 0));
				StoreEdge(vEdge0, V0V1);

				__m128i vEdge1 = _mm_castps_si128(_mm256_extractf128_ps(_mm256_castsi256_ps(vE0E1Encoded), 1));
				StoreEdge(vEdge1, V0V2);

				// store edge lengths as 16-bit floats
				__m128i vEdgeLengthsHalf = _mm_cvtps_ph(vEdgeLengths, 0);
				int lengths = _mm_cvtsi128_si32(vEdgeLengthsHalf);
				memcpy(&EdgeLengths, &lengths, sizeof(int));

				// remember sign of z component
				int cmpMask = _mm256_movemask_ps(_mm256_cmp_ps(vE0E1, _mm256_setzero_ps(), _CMP_GE_OQ));
				int isPos0 = (cmpMask & (1 << 2)) == (1 << 2);
				int isPos1 = (cmpMask & (1 << 6)) == (1 << 6);

				// clear the sign bits
				EmissiveFactor_Signs &= ~((1u << V0V1SignBit) | (1u << V0V2SignBit));

				EmissiveFactor_Signs = EmissiveFactor_Signs | (isPos0 << V0V1SignBit);
				EmissiveFactor_Signs = EmissiveFactor_Signs | (isPos1 << V0V2SignBit);
#else
				Vtx1 = Math::storeFloat3(v1);
				Vtx2 = Math::storeFloat3(v2);
#endif
			}

			ZetaInline void __vectorcall LoadVertices(__m128& v0, __m128& v1, __m128& v2)
			{
				__m128 vOne = _mm_set1_ps(1.0f);

#if ENCODE_EMISSIVE_POS == 1
				bool signIsPosV1 = EmissiveFactor_Signs & (1u << V0V1SignBit);
				bool signIsPosV2 = EmissiveFactor_Signs & (1u << V0V2SignBit);
				__m128 V1Mask = _mm_setr_ps(1.0f, 1.0f, signIsPosV1 ? 1.0f : -1.0f, 1.0f);
				__m128 V2Mask = _mm_setr_ps(1.0f, 1.0f, signIsPosV2 ? 1.0f : -1.0f, 1.0f);

				alignas(16) int32_t packed[4] = { int32_t(V0V1.x), int32_t(V0V1.y),
					int32_t(V0V2.x), int32_t(V0V2.y) };

				// decode SNORM-16
				__m128 vE0E1 = _mm_cvtepi32_ps(_mm_load_si128(reinterpret_cast<__m128i*>(packed)));
				vE0E1 = _mm_div_ps(vE0E1, _mm_set1_ps((1 << 15) - 1));

				// convert length to float
				__m128i vLengthsHalf = _mm_cvtsi32_si128(EdgeLengths.x | (EdgeLengths.y << 16));
				__m128 vLengths = _mm_cvtph_ps(vLengthsHalf);

				// z = sqrt(1 - x * x - y * y)
				__m128 vZs = _mm_mul_ps(vE0E1, vE0E1);
				vZs = _mm_hadd_ps(vZs, vZs);
				vZs = _mm_sub_ps(vOne, vZs);
				// due to precision loss, result could be negative, which leads to NaN in sqrt
				vZs = _mm_max_ps(vZs, _mm_setzero_ps());
				vZs = _mm_sqrt_ps(vZs);

				// restore the z component
				__m128 vV1 = _mm_insert_ps(vE0E1, vZs, 0x20);
				// and restore its sign
				vV1 = _mm_mul_ps(vV1, V1Mask);
				// interpolate
				__m128 vV0 = Math::loadFloat3(Vtx0);
				vV1 = _mm_fmadd_ps(vV1, _mm_broadcastss_ps(vLengths), vV0);
				v1 = vV1;

				__m128 vV2 = _mm_insert_ps(_mm_shuffle_ps(vE0E1, vE0E1, V_SHUFFLE_XYZW(2, 3, 3, 3)), vZs, 0x60);
				vV2 = _mm_mul_ps(vV2, V2Mask);
				vV2 = _mm_fmadd_ps(vV2, _mm_shuffle_ps(vLengths, vLengths, V_SHUFFLE_XYZW(1, 1, 1, 1)), vV0);
				v2 = vV2;
#else
				__m128 vV0 = Math::loadFloat3(Vtx0);
				__m128 vV1 = Math::loadFloat3(Vtx1);
				__m128 vV2 = Math::loadFloat3(Vtx2);
#endif
				// set v[3] = 1
				v0 = _mm_insert_ps(vV0, vOne, 0x30);
				v1 = _mm_insert_ps(vV1, vOne, 0x30);
				v2 = _mm_insert_ps(vV2, vOne, 0x30);
			}

			ZetaInline bool IsIDPatched()
			{
				return EmissiveFactor_Signs & (1u << TriIDPatchedBit);
			}

			ZetaInline void ResetID(uint32_t id)
			{
				EmissiveFactor_Signs |= (1u << TriIDPatchedBit);
				ID = id;
			}

			ZetaInline void __vectorcall StoreEdge(__m128i vEdge, snorm2_& e)
			{
				vEdge = _mm_and_si128(vEdge, _mm_set1_epi32(0xffff));

				alignas(16) uint32_t a[4];
				_mm_store_si128(reinterpret_cast<__m128i*>(a), vEdge);

				e.x = static_cast<int16_t>(a[0]);
				e.y = static_cast<int16_t>(a[1]);
			}
#endif

#ifndef __cplusplus
			float3 V1()
			{
#if ENCODE_EMISSIVE_POS == 1
				float sign = (EmissiveFactor_Signs & (1u << V0V1SignBit)) ? 1.0f : -1.0f;
				float3 v0v1 = float3(V0V1 / float((1 << 15) - 1), 0.0f);
				float z2 = 1 - v0v1.x * v0v1.x - v0v1.y * v0v1.y;
				z2 = max(0, z2);
				v0v1.z = sqrt(z2);
				v0v1.z *= sign;
				return mad(v0v1, EdgeLengths.x, Vtx0);
#else
				return Vtx1;
#endif
			}

			float3 V2()
			{
#if ENCODE_EMISSIVE_POS == 1
				float sign = (EmissiveFactor_Signs & (1u << V0V2SignBit)) ? 1.0f : -1.0f;
				float3 v0v2 = float3(V0V2 / float((1 << 15) - 1), 0.0f);
				float z2 = 1 - v0v2.x * v0v2.x - v0v2.y * v0v2.y;
				z2 = max(0, z2);
				v0v2.z = sqrt(z2);
				v0v2.z *= sign;
				return mad(v0v2, EdgeLengths.y, Vtx0);
#else
				return Vtx2;
#endif			
			}

			uint16_t GetEmissiveTex()
			{
				return uint16_t(EmissiveTex_Strength & 0xffff);
			}

			half GetEmissiveStrength()
			{
				return asfloat16(uint16_t(EmissiveTex_Strength >> 16));
			}

			bool IsDoubleSided()
			{
				return EmissiveFactor_Signs & (1u << DoubleSidedBit);
			}
#endif
		};
	}
#ifdef __cplusplus
}
#endif

#endif
