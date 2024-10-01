#ifndef RTCOMMON_H
#define RTCOMMON_H

#include "../Core/Material.h"

#ifdef __cplusplus
#include "../Math/VectorFuncs.h"
#else
#include "../../ZetaRenderPass/Common/Math.hlsli"
#endif

// When enabled, instead of storing vertex positions directly, store normalized 
// edge vector (e.g. v0v1) along with the corresponding edge length. Position can 
// then be reconstructed as e.g. v1 = v0 + v0v1 * ||v1 - v0||. Saves 12 bytes per 
// triangle.
#define ENCODE_EMISSIVE_POS 1

// Use 16-bit floats for storing triangle uv coordinates. Emissive textures tend 
// to have lower resolutions, so the loss of precision might be acceptable. Saves
// 12 bytes per triangle.
#define EMISSIVE_UV_HALF 1

#if EMISSIVE_UV_HALF == 1
#define EMISSIVE_UV_TYPE half2_
#else
#define EMISSIVE_UV_TYPE float2_
#endif

// From DXR docs:
// "Meshes present in an acceleration structure can be subdivided into groups
// based on a specified 8-bit mask value. During ray traversal, instance mask from 
// the ray and corresponding mask from each mesh are ANDed together. Mesh is skipped
// if the result is zero".
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
            unorm4_ Rotation;
            half3_ Scale;
            uint16_t MatIdx;
            uint32_t BaseEmissiveTriOffset;

            float3_ Translation;
            unorm4_ PrevRotation;
            half3_ PrevScale;
            half3_ dTranslation;

            // inline alpha stuff to avoid loading material data in anyhit shaders
            uint16_t BaseColorTex;
            uint16_t AlphaFactor_Cutoff;
        };

        struct EmissiveTriangle
        {
            static const uint32_t TriIDPatchedBit = 24;
            static const uint32_t DoubleSidedBit = 25;

#ifdef __cplusplus
            EmissiveTriangle() = default;
            EmissiveTriangle(const Math::float3& vtx0, const Math::float3& vtx1, const Math::float3& vtx2,
                const Math::float2& uv0, const Math::float2& uv1, const Math::float2& uv2,
                uint32_t emissiveFactorRGB8, uint32_t emissiveTex, Math::half emissiveStr, 
                uint32_t triIdx, bool doubleSided = true)
                : ID(triIdx),
                UV0(uv0),
                UV1(uv1),
                UV2(uv2)
            {
                __m128 v0 = Math::loadFloat3(const_cast<Math::float3&>(vtx0));
                __m128 v1 = Math::loadFloat3(const_cast<Math::float3&>(vtx1));
                __m128 v2 = Math::loadFloat3(const_cast<Math::float3&>(vtx2));
                StoreVertices(v0, v1, v2);

                PackedA = emissiveFactorRGB8 & 0xffffff;
                PackedA |= (doubleSided << DoubleSidedBit);
                PackedA |= ((emissiveStr.x & 0xf) << 28);
                PackedB = emissiveTex | (uint32_t(emissiveStr.x) << Material::NUM_TEXTURE_BITS);
            }
#endif

            float3_ Vtx0;

#if ENCODE_EMISSIVE_POS == 1
            unorm2_ V0V1;
            unorm2_ V0V2;
            half2_ EdgeLengths;
#else
            float3_ Vtx1;
            float3_ Vtx2;
#endif

            // Initially, triangle index within the mesh, then changed to tri ID
            uint32_t ID;
            // [0 - 24): Emissive factor, [24 - 32): flags
            uint32_t PackedA;
            // [0 - 16): Texture, [16 - 32): strength
            uint32_t PackedB;

            EMISSIVE_UV_TYPE UV0;
            EMISSIVE_UV_TYPE UV1;
            EMISSIVE_UV_TYPE UV2;

            bool IsDoubleSided() CONST
            {
                return PackedA & (1u << DoubleSidedBit);
            }

            half_ GetStrength() CONST
            {
                uint16_t str = uint16_t(PackedB >> Material::NUM_TEXTURE_BITS);
#ifdef __cplusplus
                return Math::half::asfloat16(str);
#else
                return asfloat16(str);
#endif
            }

            float3_ GetFactor() CONST
            {
                return Math::UnpackRGB8(PackedA);
            }

            uint32_t GetTex() CONST
            {
                return PackedB & Material::TEXTURE_MASK;
            }

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
                //  = normalize(v1 - v0, v2 - v0)
                __m256 vE0E1Normalized = _mm256_div_ps(vE0E1, vEdgeLengthsSplatted);

                // Octahedral encoding
                __m128 vE0 = _mm256_extractf128_ps(vE0E1Normalized, 0);
                vE0 = Math::encode_octahedral(vE0);
                __m128 vE1 = _mm256_extractf128_ps(vE0E1Normalized, 1);
                vE1 = Math::encode_octahedral(vE1);
                vE0E1Normalized = _mm256_insertf128_ps(_mm256_castps128_ps256(vE0), vE1, 1);

                // Encode using 16-bit UNORMs
                __m256 vHalf = _mm256_set1_ps(0.5f);
                // [-1, 1] -> [0, 1]
                vE0E1Normalized = _mm256_fmadd_ps(vE0E1Normalized, vHalf, vHalf);
                __m256 vMax = _mm256_set1_ps((1 << 16) - 1);
                __m256 vTemp = _mm256_mul_ps(vE0E1Normalized, vMax);
                __m256i vE0E1Encoded = _mm256_cvtps_epi32(vTemp);

                // Store normalized edges
                __m128i vEdge0 = _mm_castps_si128(_mm256_extractf128_ps(_mm256_castsi256_ps(vE0E1Encoded), 0));
                StoreEdge(vEdge0, V0V1);

                __m128i vEdge1 = _mm_castps_si128(_mm256_extractf128_ps(_mm256_castsi256_ps(vE0E1Encoded), 1));
                StoreEdge(vEdge1, V0V2);

                // Store edge lengths as 16-bit floats
                __m128i vEdgeLengthsHalf = _mm_cvtps_ph(vEdgeLengths, 0);
                int lengths = _mm_cvtsi128_si32(vEdgeLengthsHalf);
                memcpy(&EdgeLengths, &lengths, sizeof(int));
#else
                Vtx1 = Math::storeFloat3(v1);
                Vtx2 = Math::storeFloat3(v2);
#endif
            }

            ZetaInline void __vectorcall LoadVertices(__m128& v0, __m128& v1, __m128& v2)
            {
                __m128 vOne = _mm_set1_ps(1.0f);

#if ENCODE_EMISSIVE_POS == 1

                alignas(16) int32_t packed[4] = { int32_t(V0V1.x), int32_t(V0V1.y),
                    int32_t(V0V2.x), int32_t(V0V2.y) };

                // Decode UNORM-16
                __m128 vE0E1 = _mm_cvtepi32_ps(_mm_load_si128(reinterpret_cast<__m128i*>(packed)));
                vE0E1 = _mm_div_ps(vE0E1, _mm_set1_ps((1 << 16) - 1));
                // [0, 1] -> [-1, 1]
                vE0E1 = _mm_fmadd_ps(vE0E1, _mm_set1_ps(2.0f), _mm_set1_ps(-1.0f));

                // Convert length to float
                __m128i vLengthsHalf = _mm_cvtsi32_si128(EdgeLengths.x | (EdgeLengths.y << 16));
                __m128 vLengths = _mm_cvtph_ps(vLengthsHalf);

                // Interpolate
                __m128 vV0 = Math::loadFloat3(Vtx0);

                __m128 vV1 = Math::decode_octahedral(vE0E1);
                vV1 = _mm_fmadd_ps(vV1, _mm_broadcastss_ps(vLengths), vV0);
                v1 = vV1;

                __m128 vV2 = Math::decode_octahedral(_mm_movehl_ps(vE0E1, vE0E1));
                vV2 = _mm_fmadd_ps(vV2, _mm_shuffle_ps(vLengths, vLengths, V_SHUFFLE_XYZW(1, 1, 1, 1)), vV0);
                v2 = vV2;
#else
                __m128 vV0 = Math::loadFloat3(Vtx0);
                __m128 vV1 = Math::loadFloat3(Vtx1);
                __m128 vV2 = Math::loadFloat3(Vtx2);
#endif
                // Set v[3] = 1
                v0 = _mm_insert_ps(vV0, vOne, 0x30);
                v1 = _mm_insert_ps(vV1, vOne, 0x30);
                v2 = _mm_insert_ps(vV2, vOne, 0x30);
            }

            ZetaInline bool IsIDPatched()
            {
                return PackedA & (1u << TriIDPatchedBit);
            }

            ZetaInline void ResetID(uint32_t id)
            {
                PackedA |= (1u << TriIDPatchedBit);
                ID = id;
            }

            ZetaInline void __vectorcall StoreEdge(__m128i vEdge, unorm2_& e)
            {
                vEdge = _mm_and_si128(vEdge, _mm_set1_epi32(0xffff));

                alignas(16) uint32_t a[4];
                _mm_store_si128(reinterpret_cast<__m128i*>(a), vEdge);

                e.x = static_cast<uint16_t>(a[0]);
                e.y = static_cast<uint16_t>(a[1]);
            }

            ZetaInline void SetStrength(float newStrength)
            {
                Math::half h(newStrength);
                SetStrength(h);
            }

            ZetaInline void SetStrength(Math::half newStrength)
            {
                PackedB = (PackedB & Material::TEXTURE_MASK) | 
                    (uint32_t(newStrength.x) << Material::NUM_TEXTURE_BITS);
            }

            ZetaInline void SetEmissiveFactor(uint32_t newFactorRGB8)
            {
                PackedA = newFactorRGB8 | (PackedA & Material::UPPER_8_BITS_MASK);
            }
#endif
        };

        // Given discrete probability distribution P with N outcomes, such that for outcome i and random variable x,
        //      P[i] = P[x = i],
        // 
        // alias table is a lookup table of length N for P. To draw samples from P, draw a discrete uniform sample x 
        // in [0, N), then
        // 
        // 1. Draw another uniform sample u in [0, 1)
        // 2. If u <= AliasTable[x].P_Curr, return x
        // 3. Return AliasTable[x].Alias
        struct EmissiveLumenAliasTableEntry
        {
            // Cache the probabilities for both outcomes to avoid another (random) memory access at the
            // cost of extra storage
            float CachedP_Orig;
            float CachedP_Alias;
            float P_Curr;
            uint32_t Alias;
        };

        struct PresampledEmissiveTriangle
        {
            float3_ pos;
            unorm2_ normal;
            float pdf;
            uint32_t ID;
            uint32_t idx;
            unorm2_ bary;
            half3_ le;
            uint16_t twoSided;
        };

        struct VoxelSample
        {
            float3_ pos;
            unorm2_ normal;
            float pdf;
            uint32_t ID;
            half3_ le;
            uint16_t twoSided;
        };
    }
#ifdef __cplusplus
}
#endif

#endif
