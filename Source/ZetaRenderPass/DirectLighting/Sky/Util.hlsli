#ifndef SKY_DI_UTIL_H
#define SKY_DI_UTIL_H

#include "../../Common/LightSource.hlsli"

namespace SkyDI_Util
{
    struct SkyIncidentRadiance 
    {
        static SkyIncidentRadiance Init(uint descHeapOffset)
        {
            SkyIncidentRadiance ret;
            ret.skyViewDescHeapOffset = descHeapOffset;
            return ret;
        }

        float3 operator()(float3 w)
        {
            return Light::Le_Sky(w, skyViewDescHeapOffset);
        }

        uint skyViewDescHeapOffset;
    };

    float3 WorldToTangentFrame(float3 normal, float3 w)
    {
        float3 b1;
        float3 b2;
        Math::revisedONB(normal, b1, b2);
        float3x3 worldToLocal = float3x3(b1, b2, normal);

        return mul(worldToLocal, w);
    }

    float3 FromTangentFrameToWorld(float3 normal, float3 w_local)
    {
        float3 b1;
        float3 b2;
        Math::revisedONB(normal, b1, b2);
        float3x3 localToWorld_T = float3x3(b1, b2, normal);

        return mul(w_local, localToWorld_T);
    }
}

#endif