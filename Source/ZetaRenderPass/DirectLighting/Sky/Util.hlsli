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
}

#endif