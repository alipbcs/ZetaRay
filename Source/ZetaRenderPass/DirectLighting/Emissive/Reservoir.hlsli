#ifndef RESTIR_DI_RESERVOIR_H
#define RESTIR_DI_RESERVOIR_H

#include "DirectLighting_Common.h"
#include "../../Common/Sampling.hlsli"

namespace RDI_Util
{
    struct Reservoir
    {
        static Reservoir Init()
        {
            Reservoir ret;

            ret.le = 0;
            ret.M = 0;
            ret.w_sum = 0;
            ret.W = 0;
            ret.lightIdx = UINT32_MAX;
            ret.bary = 0;

            ret.target = 0;
            ret.lightID = UINT32_MAX;

            return ret;
        }

        bool Update(float weight, float3 le, uint lightIdx, float2 bary, inout RNG rng)
        {
            if(isnan(weight))
                return false;

            this.M += 1;

            if(weight == 0)
                return false;

            this.w_sum += weight;

            if (rng.Uniform() < (weight / this.w_sum))
            {
                this.le = le;
                this.lightIdx = lightIdx;
                this.bary = bary;

                return true;
            }

            return false;
        }

        static Reservoir Load(uint2 DTid, uint inputAIdx, uint inputBIdx)
        {
            Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
            Texture2D<float2> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];

            const uint4 resA = g_reservoir_A[DTid];
            const float2 resB = g_reservoir_B[DTid];
            float3 le = asfloat16(uint16_t3(resA.y & 0xffff, resA.y >> 16, resA.z & 0xffff));
            float2 bary = Math::DecodeUNorm2(uint16_t2(resA.x & 0xffff, resA.x >> 16));

            Reservoir ret;
            ret.w_sum = resB.x;
            ret.W = resB.y;
            ret.le = le;
            ret.lightIdx = resA.w;
            ret.bary = bary;
            ret.M = uint16_t(resA.z >> 16);

            ret.lightPos = 0;

            return ret;
        }

        void LoadTarget(uint2 DTid, uint uavIdx)
        {
            RWTexture2D<float4> g_target = ResourceDescriptorHeap[uavIdx];
            this.target = g_target[DTid].xyz;
        }

        void WriteTarget(uint2 DTid, uint uavIdx)
        {
            RWTexture2D<float4> g_target = ResourceDescriptorHeap[uavIdx];
            this.target = Math::Sanitize(this.target);
            g_target[DTid].xyz = this.target;
        }

        void Write(uint2 DTid, uint outputAIdx, uint outputBIdx, uint16 M_max)
        {
            RWTexture2D<uint4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
            RWTexture2D<float2> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];

            uint16_t3 le = asuint16(half3(this.le));
            uint16_t M_capped = min(this.M, M_max);
            uint16_t2 bary = Math::EncodeAsUNorm2(this.bary);

            uint a_x = (uint(bary.y) << 16) | bary.x;
            uint a_y = (uint(le.y) << 16) | le.x;
            uint a_z = (uint(M_capped) << 16) | le.z;
            uint a_w = this.lightIdx;

            g_outReservoir_A[DTid] = uint4(a_x, a_y, a_z, a_w);
            g_outReservoir_B[DTid] = float2(this.w_sum, this.W);
        }

        float w_sum;
        float W;
        float3 le;
        uint lightIdx;
        float2 bary;
        uint16_t M;

        float3 target;
        uint lightID;
        float3 lightPos;
        float3 lightNormal;
        bool doubleSided;

        float rayT;
    };
}

#endif