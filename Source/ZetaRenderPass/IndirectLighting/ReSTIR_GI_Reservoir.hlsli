#ifndef RESTIR_GI_RESERVOIR_H
#define RESTIR_GI_RESERVOIR_H

#include "IndirectLighting_Common.h"
#include "../Common/Sampling.hlsli"
#include "ReSTIR_GI_Common.hlsli"

namespace RGI_Util
{
    struct Reservoir
    {
        static Reservoir Init()
        {
            Reservoir res;

            res.pos = FLT_MAX.xxx;
            res.normal = 0;
            res.Lo = 0.0.xxx;
            res.M = 0;
            res.w_sum = 0;
            res.W = 0;
            res.ID = uint(-1);
            
            return res;
        }

        static Reservoir Init(float3 vtxPos, uint16_t m, float3 Lo, uint hitID)
        {
            Reservoir res;

            res.pos = vtxPos;
            res.Lo = Lo;
            res.ID = hitID;
            res.M = m;
            res.normal = 0;
            res.w_sum = 0;
            res.W = 0;

            return res;
        }
        
        bool Update(float weight, float3 vtxPos, float3 vtxNormal, uint vtxID, float3 vtxLo, float3 target, 
            inout RNG rng)
        {
            if(isnan(weight))
                return false;

            this.w_sum += weight;
            this.M += 1;

            if (rng.Uniform() < (weight / max(1e-6f, this.w_sum)))
            {
                this.pos = vtxPos;
                this.normal = vtxNormal;
                this.ID = vtxID;
                this.Lo = vtxLo;
                this.target_z = target;

                return true;
            }

            return false;
        }

        bool IsValid()
        {
            return ID != uint(-1);
        }

        float3 pos;
        float3 Lo;

        float3 normal;
        float W;
        float w_sum;
        uint ID;

        float3 target_z;
        half M;
    };

    Reservoir PartialReadReservoir_Reuse(uint2 DTid, uint inputAIdx, uint inputBIdx)
    {
        Texture2D<float4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
        Texture2D<half4> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
        const float4 resA = g_reservoir_A[DTid];
        const half4 resB = g_reservoir_B[DTid];

        float3 pos = resA.xyz;
        uint hitID = asuint(resA.w);
        float3 Lo = resB.xyz;
        uint16_t M = resB.w;

        return Reservoir::Init(pos, M, Lo, hitID);
    }

    void PartialReadReservoir_ReuseRest(uint2 DTid, uint inputCIdx, inout Reservoir r)
    {
        Texture2D<float4> g_reservoir_C = ResourceDescriptorHeap[inputCIdx];
        float3 resC = g_reservoir_C[DTid].xyz;
        r.w_sum = resC.x;
        r.W = resC.y;

        int n = asint(resC.z);
        int16_t2 ns = int16_t2(n & 0xffff, n >> 16);
        r.normal = Math::DecodeOct16(ns);
    }

    float3 PartialReadReservoir_Pos(uint2 DTid, uint inputAIdx)
    {
        Texture2D<float4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
        return g_reservoir_A[DTid].xyz;
    }

    void WriteReservoir(uint2 DTid, Reservoir r, uint outputAIdx, uint outputBIdx, uint outputCIdx, float M_max)
    {
        RWTexture2D<float4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
        RWTexture2D<half4> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];
        RWTexture2D<float4> g_outReservoir_C = ResourceDescriptorHeap[outputCIdx];

        int16_t2 n = Math::EncodeOct16(r.normal);
        int nu = (int(n.y) << 16) | n.x;
        half M_clamped = min(r.M, half(M_max));

        float4 outA = float4(r.pos, asfloat(r.ID));
        half4 outB = half4(r.Lo, M_clamped);
        float3 outC = float3(r.w_sum, r.W, asfloat(nu));

        g_outReservoir_A[DTid] = outA;
        g_outReservoir_B[DTid] = outB;
        g_outReservoir_C[DTid].xyz = outC;
    }
}

#endif