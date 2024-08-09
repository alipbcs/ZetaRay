#ifndef RESTIR_PT_UTIL_H
#define RESTIR_PT_UTIL_H

#include "../IndirectLighting_Common.h"
#include "Reservoir.hlsli"

namespace RPT_Util
{
    static const int SPATIAL_NEIGHBOR_OFFSET = 32;

    bool PlaneHeuristic(float3 prevPos, float3 normal, float3 pos, float linearDepth, 
        float th = MAX_PLANE_DIST_REUSE)
    {
        float planeDist = abs(dot(normal, prevPos - pos));
        bool onPlane = planeDist <= th * linearDepth;

        return onPlane;
    }

    void EncodeSorted(uint2 DTid, uint2 mappedDTid, uint descHeapIdx, uint error)
    {
        // In [-31, +31]
        int2 diff = (int2)DTid - (int2)mappedDTid;
        uint2 diffU = (uint2)(diff + 32);
        uint encoded = diffU.x | (diffU.y << 6) | (error << 14);

        RWTexture2D<uint> g_threadMap = ResourceDescriptorHeap[descHeapIdx];
        g_threadMap[mappedDTid] = encoded;
    }

    int2 DecodeSorted(uint2 DTid, uint descHeapIdx, out bool error)
    {
        Texture2D<uint> g_threadMap = ResourceDescriptorHeap[descHeapIdx];
        uint encoded = g_threadMap[DTid];

        error = (encoded >> 14) > 0;
        uint2 decoded = uint2(encoded, (encoded >> 6)) & 0x3f;
        int2 decodedI = (int2)decoded - 32;

        return (int2)DTid + decodedI;
    }

    void SuppressOutlierReservoirs(inout RPT_Util::Reservoir r)
    {
        float waveSum = WaveActiveSum(r.w_sum);
        float waveNonZeroCount = WaveActiveSum(r.w_sum != 0.0);
        float waveAvgExclusive = (waveSum - r.w_sum) / max(waveNonZeroCount - 1, 1);
        if(r.w_sum > 25 * waveAvgExclusive)
        {
            r.M = 0;
            r.w_sum = 0;
            r.W = 0;
            r.rc.Clear();
        }
    }

    void SuppressOutlierReservoirs(float waveAvgExclusive, inout RPT_Util::Reservoir r)
    {
        if(r.w_sum > 25 * waveAvgExclusive)
        {
            r.M = 0;
            r.w_sum = 0;
            r.W = 0;
            r.rc.Clear();
        }
    }

    void DebugColor(RPT_Util::Reconnection rc, uint packed, inout float3 c)
    {
        uint option = packed >> 20;
        if(option == (int) RPT_DEBUG_VIEW::NONE)
            return;

        if (option == (int) RPT_DEBUG_VIEW::K)
        {
            if(rc.Empty())
                c = float3(0, 0, 0);
            else if(rc.k == 2)
                c = float3(0.1, 0.25, 0.88);
            else if(rc.k == 3)
                c = float3(0.13, 0.55, 0.14);
            else if(rc.k == 4)
                c = float3(0.69, 0.45, 0.1);
            else if(rc.k >= 5)
                c = float3(0.88, 0.08, 0.1);
        }
        else if (option == (int) RPT_DEBUG_VIEW::CASE)
        {
            if(rc.Empty())
                c = float3(0, 0, 0);
            else if(rc.IsCase1())
                c = float3(0.85, 0.096, 0.1);
            else if(rc.IsCase2())
                c = float3(0.13, 0.6, 0.14);
            else if(rc.IsCase3())
                c = float3(0.1, 0.27, 0.888);
        }
        else if (option == (int) RPT_DEBUG_VIEW::FOUND_CONNECTION)
        {
            if(rc.Empty())
                c = float3(0, 0, 0);
            else
                c = float3(0.234, 0.12, 0.2134);
        }
        else if (option == (int) RPT_DEBUG_VIEW::RECONNECTION_LOBE_K_MIN_1)
        {
            if(rc.Empty())
                c = float3(0, 0, 0);
            else
            {
                if(rc.lobe_k_min_1 == BSDF::LOBE::DIFFUSE_R)
                    c = float3(0.384, 0.12, 0.2134);
                else if(rc.lobe_k_min_1 == BSDF::LOBE::GLOSSY_R)
                    c = float3(0.12, 0.284, 0.2134);
                else if(rc.lobe_k_min_1 == BSDF::LOBE::GLOSSY_T)
                    c = float3(0.1134, 0.12, 0.634);
                else
                    c = float3(0.25f, 0.25f, 0);
            }
        }
        else if (option == (int) RPT_DEBUG_VIEW::RECONNECTION_LOBE_K)
        {
            if(rc.Empty() || rc.IsCase3())
                c = float3(0, 0, 0);
            else
            {
                if(rc.lobe_k == BSDF::LOBE::DIFFUSE_R)
                    c = float3(0.384, 0.12, 0.2134);
                else if(rc.lobe_k == BSDF::LOBE::GLOSSY_R)
                    c = float3(0.12, 0.284, 0.2134);
                else if(rc.lobe_k == BSDF::LOBE::GLOSSY_T)
                    c = float3(0.1134, 0.12, 0.634);
                else
                    c = float3(0.25f, 0.25f, 0);
            }
        }
    }

    void WriteOutputColor(uint2 DTid, float3 li, uint packed, uint outDescHeapIdx, 
        ConstantBuffer<cbFrameConstants> g_frame,
        bool filterToBlackWhenDebugViewEnabled = true)
    {
        if((filterToBlackWhenDebugViewEnabled && ((packed >> 20) != (int) RPT_DEBUG_VIEW::NONE)))
            li = 0;

        RWTexture2D<float4> g_final = ResourceDescriptorHeap[outDescHeapIdx];
        li = any(isnan(li)) ? 0 : li;

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[DTid].rgb;
            g_final[DTid].rgb = prev + li;
        }
        else
            g_final[DTid].rgb = li;
    }

    void WriteOutputColor2(uint2 DTid, float4 val, uint packed, uint outDescHeapIdx, 
        ConstantBuffer<cbFrameConstants> g_frame,
        bool filterToBlackWhenDebugViewEnabled = true)
    {
        if((filterToBlackWhenDebugViewEnabled && ((packed >> 20) != (int) RPT_DEBUG_VIEW::NONE)))
            val.xyz = 0;

        RWTexture2D<float4> g_final = ResourceDescriptorHeap[outDescHeapIdx];
        val = any(isnan(val)) ? 0 : val;

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[DTid].rgb;
            g_final[DTid] = float4(prev + val.rgb, val.a);
        }
        else
            g_final[DTid] = val;
    }

    void ResetDeriv(uint2 DTid, uint outDescHeapIdx)
    {
        RWTexture2D<float4> g_final = ResourceDescriptorHeap[outDescHeapIdx];
        g_final[DTid].w = 0;
    }
}

#endif