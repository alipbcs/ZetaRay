#ifndef RESTIR_DI_UTIL_H
#define RESTIR_DI_UTIL_H

#include "DirectLighting_Common.h"
#include "Reservoir.hlsli"
#include "../../Common/RT.hlsli"
#include "../../Common/LightSource.hlsli"

namespace RDI_Util
{
    struct Target
    {
        static Target Init()
        {
            Target ret;

            ret.p_hat = 0.0.xxx;
            ret.lightID = UINT32_MAX;
            ret.dwdA = 0;
        
            return ret;
        }

        float3 p_hat;
        float rayT;
        uint lightID;
        float3 wi;
        float3 lightNormal;
        float3 lightPos;
        float dwdA;
    };

    struct BrdfHitInfo
    {
        uint emissiveTriIdx;
        float2 bary;
        float3 lightPos;
        float t;
    };

    struct TemporalCandidate
    {
        static TemporalCandidate Init()
        {
            TemporalCandidate ret;
            ret.valid = false;
            ret.lightIdx = UINT32_MAX;
        
            return ret;
        }

        float3 pos;
        uint lightIdx;
        float3 normal;
        float roughness;
        int16_t2 posSS;
        bool valid;
        bool metallic;
    };

    struct EmissiveData
    {
        static EmissiveData Init(Reservoir r, StructuredBuffer<RT::EmissiveTriangle> g_emissives)
        {
            EmissiveData ret;

            if(r.IsValid())
            {
                RT::EmissiveTriangle tri = g_emissives[r.LightIdx];
                ret.ID = tri.ID;

                const float3 vtx1 = Light::DecodeEmissiveTriV1(tri);
                const float3 vtx2 = Light::DecodeEmissiveTriV2(tri);
                ret.lightPos = (1.0f - r.Bary.x - r.Bary.y) * tri.Vtx0 + r.Bary.x * vtx1 + r.Bary.y * vtx2;

                ret.lightNormal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
                ret.lightNormal = dot(ret.lightNormal, ret.lightNormal) == 0 ? ret.lightNormal : normalize(ret.lightNormal);
                //ret.lightNormal = tri.IsDoubleSided() && dot(-ret.wi, ret.lightNormal) < 0 ? ret.lightNormal * -1.0f : ret.lightNormal;
                ret.doubleSided = tri.IsDoubleSided();
            }

            return ret;
        }

        void SetSurfacePos(float3 pos)
        {
            this.t = length(this.lightPos - pos);
            this.wi = (this.lightPos - pos) / this.t;
            this.lightNormal = this.doubleSided && dot(-this.wi, this.lightNormal) < 0 ? this.lightNormal * -1.0f : this.lightNormal;
        }

        float dWdA()
        {
            float cosThetaPrime = saturate(dot(this.lightNormal, -this.wi));
            cosThetaPrime = all(this.lightNormal == 0) ? 1 : cosThetaPrime;
            float dWdA = cosThetaPrime / (this.t * this.t);

            return dWdA;
        }

        float3 wi;
        float t;
        uint ID;
        float3 lightPos;
        float3 lightNormal;
        bool doubleSided;
    };

    // TODO doesn't handle transmission
    bool Visibility(RaytracingAccelerationStructure g_bvh, float3 pos, float3 wi, float rayT,
        float3 normal, uint triID)
    {
        const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = 0;
        ray.TMax = rayT;
        ray.Direction = wi;

        // Initialize
        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::NON_EMISSIVE, ray);

        // Traversal
        rayQuery.Proceed();

        // triangle intersection only when hit_t < t_max
        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), rayQuery.CommittedPrimitiveIndex());
            uint hash = RNG::PCG3d(key).x;

            return triID == hash;
        }

        return true;
    }

    bool VisibilityApproximate(RaytracingAccelerationStructure g_bvh, float3 pos, float3 wi, float rayT,
        float3 normal, uint triID, bool transmissive)
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

        // To test for occlusion against some light source at distance t_l we need to check if 
        // the ray hits any geometry for which t_hit < t_l. According to dxr specs, for any committed triangle 
        // hit, t_hit < t_max. So we set t_max = t_l and trace a ray. As any such hit indicates occlusion,
        // the RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH flag can be specified for improved 
        // perfromance. Now due to floating-point precision issues, it's possible that the first hit 
        // could be the light source itself -- t_hit ~= t_ray. In this scenario, occlusion is inconclusive 
        // as there may or may not be other occluders along the ray with t < t_hit. As an approximation, 
        // the following decreases t_l by a small amount to avoid the situation described above.
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH| 
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
            RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = wiBackface ? 3e-4 : 0;
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
            uint hash = RNG::PCG3d(key).x;

            return triID == hash;
        }

        return true;
    }

    bool FindClosestHit(float3 pos, float3 normal, float3 wi, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, out BrdfHitInfo hitInfo, bool transmissive)
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

            if (meshData.BaseEmissiveTriOffset == UINT32_MAX)
                return false;

            hitInfo.emissiveTriIdx = meshData.BaseEmissiveTriOffset + rayQuery.CommittedPrimitiveIndex();
            hitInfo.bary = rayQuery.CommittedTriangleBarycentrics();
            hitInfo.lightPos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
            hitInfo.t = rayQuery.CommittedRayT();

            return true;
        }

        return false;
    }
}

#endif