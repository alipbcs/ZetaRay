#ifndef RESTIR_GI_TRACE_H
#define RESTIR_GI_TRACE_H

#include "../Common/Math.hlsli"
#include "../Common/RT.hlsli"
#include "../Common/Common.hlsli"

namespace RGI_Trace
{
    struct HitSurface
    {
        float t;
        float2 uv;
        float3 normal;
        uint16_t matIdx;
        half lambda;
    };

    struct HitSurface_Emissive
    {
        uint emissiveTriIdx;
        float2 bary;
        float3 lightPos;
        float t;
    };

    template<bool ID>
    bool FindClosestHit(float3 pos, float3 normal, float3 wi, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, StructuredBuffer<Vertex> g_vertices, 
        StructuredBuffer<uint> g_indices, out HitSurface hitInfo, out uint hitID, bool transmissive)
    {
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

        const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);
        hitID = uint(-1);

        // Skip alpha testing for now
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        // TODO for some reason, transmission rays require extra offset
        ray.TMin = wiBackface ? 3e-4 : 0;
        ray.TMax = FLT_MAX;
        ray.Direction = wi;

        // Initialize
        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);

        // Traversal
        rayQuery.Proceed();
        
        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const uint meshIdx = rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID();
            const uint primIdx = rayQuery.CommittedPrimitiveIndex();
            const float2 bary = rayQuery.CommittedTriangleBarycentrics();

            const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(meshIdx)];

            // Skip light sources
            if (meshData.BaseEmissiveTriOffset != uint32_t(-1))
                return false;

            hitInfo.t = rayQuery.CommittedRayT();
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
            float3 v0_n = Math::DecodeOct16(V0.NormalL);
            float3 v1_n = Math::DecodeOct16(V1.NormalL);
            float3 v2_n = Math::DecodeOct16(V2.NormalL);
            float3 hitNormal = v0_n + bary.x * (v1_n - v0_n) + bary.y * (v2_n - v0_n);
            // transform normal using the inverse transpose
            // (M^-1)^T = ((SR)^-1)^T
            //          = (R^-1 S^-1)^T
            //          = (S^-1)^T (R^T)^T
            //          = S^-1 R
            hitNormal *= 1.0f / meshData.Scale;
            hitNormal = Math::RotateVector(hitNormal, q);
            hitNormal = normalize(hitNormal);
            hitInfo.normal = hitNormal;

            // Just need to apply the scale transformation, rotation and translation 
            // preserve area.
            float3 v0W = meshData.Scale * V0.PosL;
            float3 v1W = meshData.Scale * V1.PosL;
            float3 v2W = meshData.Scale * V2.PosL;
            
            float ndotwo = dot(hitNormal, -wi);
            float lambda = RT::RayCone::Lambda(v0W, v1W, v2W, V0.TexUV, V1.TexUV, V2.TexUV, ndotwo);
            hitInfo.lambda = half(lambda);

            if(ID)
            {
                uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), 
                    rayQuery.CommittedPrimitiveIndex());
                hitID = RNG::Pcg3d(key).x;
            }

            return true;
        }

        return false;
    }

    bool FindClosestHit_Emissive(float3 pos, float3 normal, float3 wi, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, out HitSurface_Emissive hitInfo, 
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

        const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);

        // Skip alpha testing for now
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = wiBackface ? 3e-4 : 0;
        ray.TMax = FLT_MAX;
        ray.Direction = wi;

        // Initialize
        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);

        // Traversal
        rayQuery.Proceed();

        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const RT::MeshInstance meshData = g_frameMeshData[rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID()];

            if (meshData.BaseEmissiveTriOffset == uint32_t(-1))
                return false;

            hitInfo.emissiveTriIdx = meshData.BaseEmissiveTriOffset + rayQuery.CommittedPrimitiveIndex();
            hitInfo.bary = rayQuery.CommittedTriangleBarycentrics();
            hitInfo.lightPos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
            hitInfo.t = rayQuery.CommittedRayT();

            return true;
        }

        return false;
    }

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
        if(triID == uint(-1))
            return false;

        if(rayT < 1e-6)
            return false;

        bool wiBackface = dot(normal, wi) <= 0;

        if(wiBackface)
        {
            if(transmissive)
                normal *= -1;
            else
                return false;
        }

        const float3 adjustedOrigin = RT::OffsetRayRTG(origin, normal);

        // To test for occlusion against some light source at distance t_l we need to check if the shadow ray 
        // towards it hits any geometry for which t_hit < t_l. According to dxr specs, for any committed triangle 
        // hit, t_hit < t_max. So we set t_max = t_l and trace a ray. As any such hit indicates occlusion,
        // the RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH flag can be specified for improved  perfromance. Now due 
        // to floating-point precision issues, it's possible that the first hit could be the light source 
        // itself -- t_hit ~= t_ray. In this scenario, occlusion is inconclusive as there may or may not be 
        // other occluders along the ray with t < t_hit. As an approximation, the following decreases t_l by 
        // a small amount to avoid the situation described above.
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH| 
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
            RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = wiBackface ? 3e-4f : 0;
        ray.TMax = 0.995f * rayT;
        ray.Direction = wi;

        // Initialize
        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::NON_EMISSIVE, ray);

        // Traversal
        rayQuery.Proceed();

        // triangle intersection only when hit_t < t_max
        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), rayQuery.CommittedPrimitiveIndex());
            uint hash = RNG::Pcg3d(key).x;

            return triID == hash;
        }

        return true;
    }
}

#endif