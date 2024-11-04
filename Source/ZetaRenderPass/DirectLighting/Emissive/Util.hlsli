#ifndef RESTIR_DI_UTIL_H
#define RESTIR_DI_UTIL_H

#include "DirectLighting_Common.h"
#include "Reservoir.hlsli"
#include "../../Common/LightSource.hlsli"
#include "../../Common/RayQuery.hlsli"

namespace RDI_Util
{
    struct EmissiveData
    {
        static EmissiveData Init(uint lightIdx, float2 bary, StructuredBuffer<RT::EmissiveTriangle> g_emissives)
        {
            EmissiveData ret;

            RT::EmissiveTriangle tri = g_emissives[lightIdx];
            ret.ID = tri.ID;

            const float3 vtx1 = Light::DecodeEmissiveTriV1(tri);
            const float3 vtx2 = Light::DecodeEmissiveTriV2(tri);
            ret.lightPos = (1.0f - bary.x - bary.y) * tri.Vtx0 + bary.x * vtx1 + bary.y * vtx2;

            ret.lightNormal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
            ret.lightNormal = dot(ret.lightNormal, ret.lightNormal) == 0 ? ret.lightNormal : 
                normalize(ret.lightNormal);
            // ret.lightNormal = tri.IsDoubleSided() && dot(-ret.wi, ret.lightNormal) < 0 ? 
            //     ret.lightNormal * -1.0f : ret.lightNormal;
            ret.doubleSided = tri.IsDoubleSided();

            return ret;
        }

        void SetSurfacePos(float3 pos)
        {
            this.wi = this.lightPos - pos;
            this.t = dot(this.wi, this.wi) == 0 ? 0 : length(wi);
            this.wi = this.t == 0 ? 0 : this.wi / this.t;
            this.lightNormal = this.doubleSided && dot(-this.wi, this.lightNormal) < 0 ? 
                -this.lightNormal : this.lightNormal;
        }

        float dWdA()
        {
            float cosThetaPrime = saturate(dot(this.lightNormal, -this.wi));
            float dWdA = this.t == 0 ? 0 : cosThetaPrime / (this.t * this.t);

            return dWdA;
        }

        float3 wi;
        float t;
        uint ID;
        float3 lightPos;
        float3 lightNormal;
        bool doubleSided;
    };

    struct BSDFHitInfo
    {
        uint emissiveTriIdx;
        float2 bary;
        float3 lightPos;
        float t;
        bool hit;
    };

    BSDFHitInfo FindClosestHit(float3 pos, float3 normal, float3 wi, RaytracingAccelerationStructure g_bvh, 
        StructuredBuffer<RT::MeshInstance> g_frameMeshData, bool transmissive)
    {
        BSDFHitInfo ret;
        ret.hit = false;

        float ndotwi = dot(normal, wi);
        if(ndotwi == 0)
            return ret;

        bool wiBackface = ndotwi < 0;

        if(wiBackface)
        {
            if(transmissive)
                normal *= -1;
            else
                return ret;
        }

        const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> rayQuery;

        RayDesc ray;
        ray.Origin = adjustedOrigin;
        ray.TMin = wiBackface ? 3e-4 : 0;
        ray.TMax = FLT_MAX;
        ray.Direction = wi;

        rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
        rayQuery.Proceed();

        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const uint meshIdx = rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID();
            const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(meshIdx)];

            if (meshData.BaseEmissiveTriOffset == UINT32_MAX)
                return ret;

            ret.emissiveTriIdx = meshData.BaseEmissiveTriOffset + rayQuery.CommittedPrimitiveIndex();
            ret.bary = rayQuery.CommittedTriangleBarycentrics();
            ret.lightPos = mad(rayQuery.CommittedRayT(), rayQuery.WorldRayDirection(), 
                rayQuery.WorldRayOrigin());
            ret.t = rayQuery.CommittedRayT();
            ret.hit = true;

            return ret;
        }

        return ret;
    }
}

#endif