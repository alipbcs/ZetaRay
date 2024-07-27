#ifndef RESTIR_RT_H
#define RESTIR_RT_H

#include "../Common/RT.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/BSDF.hlsli"

#define T_MIN_REFL_RAY 1e-6
#define T_MIN_TR_RAY 5e-5

namespace ReSTIR_RT
{
    struct Hit
    {
        template<bool ID>
        static Hit FindClosest(float3 pos, float3 normal, float3 wi, RaytracingAccelerationStructure g_bvh, 
            StructuredBuffer<RT::MeshInstance> g_frameMeshData, StructuredBuffer<Vertex> g_vertices, 
            StructuredBuffer<uint> g_indices, bool transmissive)
        {
            Hit ret;
            ret.hit = false;
            ret.ID = UINT32_MAX;

            float ndotwi = dot(normal, wi);
            if(ndotwi == 0)
                return ret;

            bool wiBackface = ndotwi < 0;
            if(wiBackface)
            {
                if(!transmissive)
                    return ret;

                normal = -normal;
            }

            const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);

            // Skip alpha testing for now
            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> rayQuery;

            RayDesc ray;
            ray.Origin = adjustedOrigin;
            ray.TMin = wiBackface ? T_MIN_TR_RAY : T_MIN_REFL_RAY;
            ray.TMax = FLT_MAX;
            ray.Direction = wi;

            rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
            rayQuery.Proceed();
            
            if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                const uint meshIdx = rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID();
                const float2 bary = rayQuery.CommittedTriangleBarycentrics();

                const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(meshIdx)];

                ret.t = rayQuery.CommittedRayT();
                ret.matIdx = meshData.MatIdx;

                uint tri = rayQuery.CommittedPrimitiveIndex() * 3;
                tri += meshData.BaseIdxOffset;
                uint i0 = g_indices[NonUniformResourceIndex(tri)] + meshData.BaseVtxOffset;
                uint i1 = g_indices[NonUniformResourceIndex(tri + 1)] + meshData.BaseVtxOffset;
                uint i2 = g_indices[NonUniformResourceIndex(tri + 2)] + meshData.BaseVtxOffset;

                Vertex V0 = g_vertices[NonUniformResourceIndex(i0)];
                Vertex V1 = g_vertices[NonUniformResourceIndex(i1)];
                Vertex V2 = g_vertices[NonUniformResourceIndex(i2)];

                float4 q = Math::DecodeSNorm4(meshData.Rotation);
                // Due to quantization, it's necessary to renormalize
                q = normalize(q);

                // Texture UV coords
                float2 uv = V0.TexUV + bary.x * (V1.TexUV - V0.TexUV) + bary.y * (V2.TexUV - V0.TexUV);
                ret.uv = uv;

                // normal
                float3 v0_n = Math::DecodeOct32(V0.NormalL);
                float3 v1_n = Math::DecodeOct32(V1.NormalL);
                float3 v2_n = Math::DecodeOct32(V2.NormalL);
                float3 hitNormal = v0_n + bary.x * (v1_n - v0_n) + bary.y * (v2_n - v0_n);
                // transform normal using the inverse transpose
                // (M^-1)^T = ((SR)^-1)^T
                //          = (R^-1 S^-1)^T
                //          = (S^-1)^T (R^T)^T
                //          = S^-1 R
                const float3 scaleInv = 1.0f / meshData.Scale;
                hitNormal *= scaleInv;
                hitNormal = Math::RotateVector(hitNormal, q);
                hitNormal = normalize(hitNormal);
                ret.normal = hitNormal;

                float3 v0W = Math::TransformTRS(V0.PosL, meshData.Translation, q, meshData.Scale);
                float3 v1W = Math::TransformTRS(V1.PosL, meshData.Translation, q, meshData.Scale);
                float3 v2W = Math::TransformTRS(V2.PosL, meshData.Translation, q, meshData.Scale);

                float3 n0W = v0_n * scaleInv;
                n0W = Math::RotateVector(n0W, q);
                n0W = normalize(n0W);

                float3 n1W = v1_n * scaleInv;
                n1W = Math::RotateVector(n1W, q);
                n1W = normalize(n1W);

                float3 n2W = v2_n * scaleInv;
                n2W = Math::RotateVector(n2W, q);
                n2W = normalize(n2W);

                ret.triDiffs = Math::TriDifferentials::Compute(v0W, v1W, v2W, 
                    n0W, n1W, n2W,
                    V0.TexUV, V1.TexUV, V2.TexUV);

                if(ID)
                {
                    uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), 
                        rayQuery.CommittedPrimitiveIndex());
                    ret.ID = RNG::PCG3d(key).x;
                }

                ret.hit = true;
            }

            return ret;
        }

        float t;
        float2 uv;
        float3 normal;
        uint ID;
        bool hit;
        Math::TriDifferentials triDiffs;
        uint16_t matIdx;
    };

    struct Hit_Emissive
    {
        static Hit_Emissive FindClosest(float3 pos, float3 normal, float3 wi, 
            RaytracingAccelerationStructure g_bvh, StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
            bool transmissive)
        {
            Hit_Emissive ret;
            ret.hit = false;
            ret.emissiveTriIdx = UINT32_MAX;

            bool wiBackface = dot(normal, wi) <= 0;
            if(wiBackface)
            {
                if(transmissive)
                    normal *= -1;
                else
                    return ret;
            }

            const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);

            // Skip alpha testing for now
            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> rayQuery;

            RayDesc ray;
            ray.Origin = adjustedOrigin;
            ray.TMin = wiBackface ? T_MIN_TR_RAY : T_MIN_REFL_RAY;
            ray.TMax = FLT_MAX;
            ray.Direction = wi;

            rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
            rayQuery.Proceed();

            if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                ret.hit = true;
                ret.bary = rayQuery.CommittedTriangleBarycentrics();
                ret.t = rayQuery.CommittedRayT();
                ret.geoIdx = rayQuery.CommittedGeometryIndex();
                ret.insID = rayQuery.CommittedInstanceID();
                ret.primIdx = rayQuery.CommittedPrimitiveIndex();
                const uint meshIdx = rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID();
                const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(meshIdx)];

                if (meshData.BaseEmissiveTriOffset == UINT32_MAX)
                    return ret;

                ret.emissiveTriIdx = meshData.BaseEmissiveTriOffset + rayQuery.CommittedPrimitiveIndex();
                ret.lightPos = mad(rayQuery.CommittedRayT(), rayQuery.WorldRayDirection(),
                    rayQuery.WorldRayOrigin());

                return ret;
            }

            return ret;
        }

        bool HitWasEmissive()
        {
            return this.emissiveTriIdx != UINT32_MAX;
        }

        Hit ToHitInfo(float3 wi, StructuredBuffer<RT::MeshInstance> g_frameMeshData, 
            StructuredBuffer<Vertex> g_vertices, 
            StructuredBuffer<uint> g_indices)
        {
            Hit hitInfo;
            hitInfo.hit = this.hit;
            hitInfo.t = this.t;

            if(!this.hit)
                return hitInfo;

            const uint meshIdx = this.geoIdx + this.insID;
            const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(meshIdx)];
            hitInfo.matIdx = meshData.MatIdx;

            uint tri = primIdx * 3;
            tri += meshData.BaseIdxOffset;
            uint i0 = g_indices[NonUniformResourceIndex(tri)] + meshData.BaseVtxOffset;
            uint i1 = g_indices[NonUniformResourceIndex(tri + 1)] + meshData.BaseVtxOffset;
            uint i2 = g_indices[NonUniformResourceIndex(tri + 2)] + meshData.BaseVtxOffset;

            Vertex V0 = g_vertices[NonUniformResourceIndex(i0)];
            Vertex V1 = g_vertices[NonUniformResourceIndex(i1)];
            Vertex V2 = g_vertices[NonUniformResourceIndex(i2)];

            float4 q = Math::DecodeSNorm4(meshData.Rotation);
            // Due to quantization, it's necessary to renormalize
            q = normalize(q);

            // Texture UV coords
            float2 uv = V0.TexUV + bary.x * (V1.TexUV - V0.TexUV) + bary.y * (V2.TexUV - V0.TexUV);
            hitInfo.uv = uv;

            // normal
            float3 v0_n = Math::DecodeOct32(V0.NormalL);
            float3 v1_n = Math::DecodeOct32(V1.NormalL);
            float3 v2_n = Math::DecodeOct32(V2.NormalL);
            float3 hitNormal = v0_n + bary.x * (v1_n - v0_n) + bary.y * (v2_n - v0_n);
            // transform normal using the inverse transpose
            // (M^-1)^T = ((SR)^-1)^T
            //          = (R^-1 S^-1)^T
            //          = (S^-1)^T (R^T)^T
            //          = S^-1 R
            const float3 scaleInv = 1.0f / meshData.Scale;
            hitNormal *= scaleInv;
            hitNormal = Math::RotateVector(hitNormal, q);
            hitNormal = normalize(hitNormal);
            hitInfo.normal = hitNormal;

            float3 v0W = Math::TransformTRS(V0.PosL, meshData.Translation, q, meshData.Scale);
            float3 v1W = Math::TransformTRS(V1.PosL, meshData.Translation, q, meshData.Scale);
            float3 v2W = Math::TransformTRS(V2.PosL, meshData.Translation, q, meshData.Scale);

            float3 n0W = v0_n * scaleInv;
            n0W = Math::RotateVector(n0W, q);
            n0W = normalize(n0W);

            float3 n1W = v1_n * scaleInv;
            n1W = Math::RotateVector(n1W, q);
            n1W = normalize(n1W);

            float3 n2W = v2_n * scaleInv;
            n2W = Math::RotateVector(n2W, q);
            n2W = normalize(n2W);

            hitInfo.triDiffs = Math::TriDifferentials::Compute(v0W, v1W, v2W, 
                n0W, n1W, n2W,
                V0.TexUV, V1.TexUV, V2.TexUV);

            uint3 key = uint3(this.geoIdx, this.insID, this.primIdx);
            hitInfo.ID = RNG::PCG3d(key).x;

            return hitInfo;
        }

        bool hit;
        float t;
        uint geoIdx;
        uint insID;
        uint primIdx;
        uint emissiveTriIdx;
        float2 bary;
        float3 lightPos;
    };

    // For ray r = origin + t * wi, returns whether there's a hit with t in [0, +inf)
    bool Visibility_Ray(float3 origin, float3 wi, float3 normal, RaytracingAccelerationStructure g_bvh, 
        bool transmissive)
    {
        bool wiBackface = dot(normal, wi) <= 0;
        if(wiBackface)
        {
            if(transmissive)
                normal *= -1;
            else
                return false;
        }

        float3 adjustedOrigin = RT::OffsetRayRTG(origin, normal);
        // adjustedOrigin.y = max(adjustedOrigin.y, 5e-2);

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
                RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = lerp(0, 8e-5, dot(normal, wi));
        ray.TMax = FLT_MAX;
        ray.Direction = wi;
        
        // initialize
        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
        
        // traversal
        rayQuery.Proceed();

        // light source is occluded
        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            return false;

        return true;
    }

    // For ray r = origin + t * wi, returns whether there's a hit with t in [0, rayT)
    bool Visibility_Segment(float3 origin, float3 wi, float rayT, float3 normal, uint triID, 
        RaytracingAccelerationStructure g_bvh, bool transmissive)
    {
        if(triID == UINT32_MAX)
            return false;

        if(rayT < 1e-6)
            return false;

        float ndotwi = dot(normal, wi);
        if(ndotwi == 0)
            return false;

        bool wiBackface = ndotwi < 0;
        if(wiBackface)
        {
            if(transmissive)
                normal *= -1;
            else
                return false;
        }

        const float3 adjustedOrigin = RT::OffsetRayRTG(origin, normal);

#if APPROXIMATE_EMISSIVE_SHADOW_RAY == 1
        // To test for occlusion against some light source at distance t_l, we need to 
        // see if there are any occluders with t_hit < t_l. According to dxr specs, for 
        // any committed triangle hit, t_hit < t_max. So we set t_max = t_l and trace a 
        // shadow ray. As any such hit indicates occlusion, the 
        // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH flag can be specified for improved 
        // performance. Now due to floating-point precision issues, it's possible that the 
        // first hit could turn out to be the light source itself -- t_hit ~= t_l. In this 
        // scenario, occlusion is inconclusive as there may or may not be other occluders 
        // along the ray with t < t_hit. As an approximation, the following uses a smaller
        // t_l to avoid the situation described above.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = 3e-6;
        ray.TMax = Math::PrevFloat32(rayT * 0.999 - Math::NextFloat32(ray.TMin));
        ray.Direction = wi;
#else
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
            RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = 3e-6;
        ray.TMax = rayT;
        ray.Direction = wi;
#endif

        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::NON_EMISSIVE, ray);
        rayQuery.Proceed();

        // triangle intersection only when hit_t < t_max
        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), 
                rayQuery.CommittedPrimitiveIndex());
            uint hash = RNG::PCG3d(key).x;

            return triID == hash;
        }

        return true;
    }

    struct AnisotropicSampler 
    {
        float3 BaseColor(uint descHeapIdx, SamplerState samp, float2 uv, 
            float2 ddx_uv, float2 ddy_uv)
        {
            BASE_COLOR_MAP g_baseColor = ResourceDescriptorHeap[descHeapIdx];
            return g_baseColor.SampleGrad(samp, uv, ddx_uv, ddy_uv).rgb;
        }

        float2 MR(uint descHeapIdx, SamplerState samp, float2 uv, 
            float2 ddx_uv, float2 ddy_uv)
        {
            METALLIC_ROUGHNESS_MAP g_mr = ResourceDescriptorHeap[descHeapIdx];
            return g_mr.SampleGrad(samp, uv, ddx_uv, ddy_uv);
        }
    };

    struct IsotropicSampler 
    {
        float3 BaseColor(uint descHeapIdx, SamplerState samp, float2 uv, 
            float2 dd_uv, float2 unused)
        {
            BASE_COLOR_MAP g_baseColor = ResourceDescriptorHeap[descHeapIdx];
            uint w;
            uint h;
            g_baseColor.GetDimensions(w, h);
            float mip = log2(max(dd_uv.x * w, dd_uv.y * h));

            return g_baseColor.SampleLevel(samp, uv, mip).rgb;
        }

        float2 MR(uint descHeapIdx, SamplerState samp, float2 uv, 
            float2 dd_uv, float2 unused)
        {
            METALLIC_ROUGHNESS_MAP g_mr = ResourceDescriptorHeap[descHeapIdx];
            uint w;
            uint h;
            g_mr.GetDimensions(w, h);
            float mip = log2(max(dd_uv.x * w, dd_uv.y * h));

            return g_mr.SampleLevel(samp, uv, mip);
        }
    };

    template<typename TexSampler>
    bool GetMaterialData(float3 wo, StructuredBuffer<Material> g_materials, 
        ConstantBuffer<cbFrameConstants> g_frame, float eta_curr, float4 uv_grads, 
        inout ReSTIR_RT::Hit hitInfo, out BSDF::ShadingData surface, out float eta,
        SamplerState samp, TexSampler ts)
    {
        const Material mat = g_materials[hitInfo.matIdx];
        const bool hitBackface = dot(wo, hitInfo.normal) < 0;
        // Set to an arbitrary value to fix compiler error
        eta = DEFAULT_ETA_I;

        // Ray hit the backside of an opaque surface, no radiance can be reflected back.
        if(!mat.IsDoubleSided() && hitBackface)
            return false;

        // Reverse normal for double-sided surfaces
        if(mat.IsDoubleSided() && hitBackface)
        {
            hitInfo.normal *= -1;
            hitInfo.triDiffs.dndu *= -1;
            hitInfo.triDiffs.dndv *= -1;
        }

        float3 baseColor = Math::UnpackRGB(mat.BaseColorFactor);
        float metalness = mat.GetMetallic();
        float roughness = mat.GetRoughnessFactor();

        if (mat.BaseColorTexture != UINT32_MAX)
        {
            uint offset = NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + 
                mat.BaseColorTexture);
            baseColor *= ts.BaseColor(offset, samp, hitInfo.uv, uv_grads.xy, uv_grads.zw);
        }

        const uint16_t metallicRoughnessTex = mat.GetMetallicRoughnessTex();

        if (metallicRoughnessTex != UINT16_MAX)
        {
            uint offset = NonUniformResourceIndex(g_frame.MetallicRoughnessMapsDescHeapOffset + 
                metallicRoughnessTex);
            float2 mr = ts.MR(offset, samp, hitInfo.uv, uv_grads.xy, uv_grads.zw);
            metalness *= mr.x;
            roughness *= mr.y;
        }

        float tr = mat.GetTransmission();
        eta = mat.GetIOR();

        // TODO surrounding medium is assumed to be always air
        float eta_t = eta_curr == ETA_AIR ? ETA_AIR : eta;
        float eta_i = eta_curr == ETA_AIR ? eta : ETA_AIR;

        surface = BSDF::ShadingData::Init(hitInfo.normal, wo, metalness >= MIN_METALNESS_METAL, 
            roughness, baseColor, eta_i, eta_t, tr >= MIN_SPEC_TR_TRANSMISSIVE);

        return true;
    }

    bool GetMaterialData(float3 wo, StructuredBuffer<Material> g_materials, 
        ConstantBuffer<cbFrameConstants> g_frame, float eta_curr, float4 uv_grads, 
        inout ReSTIR_RT::Hit hitInfo, out BSDF::ShadingData surface, out float eta,
        SamplerState samp)
    {
        AnisotropicSampler as;
        return GetMaterialData(wo, g_materials, g_frame, eta_curr, uv_grads, hitInfo,
            surface, eta, samp, as);
    }

}

#endif