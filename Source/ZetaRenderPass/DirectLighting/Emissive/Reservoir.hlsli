#ifndef RESTIR_DI_RESERVOIR_H
#define RESTIR_DI_RESERVOIR_H

#include "DirectLighting_Common.h"
#include "../../Common/Sampling.hlsli"
#include "../../Common/BSDF.hlsli"

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
            ret.halfVectorCopyShift = false;
            ret.lobe = BSDF::LOBE::ALL;

            ret.partialJacobian = 1;
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
                this.halfVectorCopyShift = false;
                this.lobe = BSDF::LOBE::ALL;

                return true;
            }

            return false;
        }

        bool Update(float weight, bool halfVecShift, float3 wi, float3 wo, float3 normal, 
            BSDF::LOBE lb, float3 le, uint lightIdx, float2 bary, inout RNG rng)
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
                this.halfVectorCopyShift = halfVecShift;
                this.lobe = lb;

                // Sample uses half-vector copy shift
                if(halfVecShift)
                {
                    float3 wh = normalize(wo + wi);
                    this.wh_local = Math::WorldToTangentFrame(normal, wh);
                    this.partialJacobian = abs(dot(wh, wo));
                }

                return true;
            }

            return false;
        }

        bool Update(float weight, bool halfVecShift, float3 wh, float whdotwo, BSDF::LOBE lb, 
            float3 le, uint lightIdx, float2 bary, inout RNG rng)
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
                this.halfVectorCopyShift = halfVecShift;
                this.lobe = lb;
                this.wh_local = wh;
                this.partialJacobian = whdotwo;

                return true;
            }

            return false;
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

        static Reservoir Load(uint2 DTid, uint inputAIdx, uint inputBIdx)
        {
            Texture2D<uint4> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
            Texture2D<float2> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];

            const uint4 resA = g_reservoir_A[DTid];
            const float2 resB = g_reservoir_B[DTid];
            const float3 le = asfloat16(uint16_t3(resA.y & 0xffff, resA.y >> 16, resA.z & 0xffff));
            const uint16_t2 bary_or_wh = uint16_t2(resA.x & 0xffff, resA.x >> 16);
            const uint metadata = resA.z >> 16;

            Reservoir ret = Reservoir::Init();
            ret.M = (uint16)(metadata & 0x1f);
            ret.w_sum = resB.x;
            ret.W = resB.y;
            ret.le = le;
            ret.lightIdx = resA.w;
            ret.bary = Math::DecodeUNorm2(bary_or_wh);
            ret.partialJacobian = 1;
            ret.lightPos = 0;

#if USE_HALF_VECTOR_COPY_SHIFT == 1
            ret.halfVectorCopyShift = (metadata >> 5) & 0x1;
            ret.lobe = BSDF::LobeFromValue((metadata >> 6) & 0x7);
            ret.wh_local = Math::DecodeOct32(bary_or_wh);
#else
            ret.halfVectorCopyShift = false;
            ret.lobe = BSDF::LOBE::ALL;
            ret.wh_local = 0;
#endif

            return ret;
        }

        void Write(uint2 DTid, uint outputAIdx, uint outputBIdx, uint16 M_max)
        {
            RWTexture2D<uint4> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
            RWTexture2D<float2> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];

            uint16_t3 le = asuint16(half3(this.le));
            uint16 M_capped = min(this.M, M_max);

#if USE_HALF_VECTOR_COPY_SHIFT == 1
            uint metadata = M_capped |
                (uint(this.halfVectorCopyShift) << 5) |
                (uint(BSDF::LobeToValue(this.lobe)) << 6);
            uint16_t2 bary_or_wh = this.halfVectorCopyShift ? 
                Math::EncodeOct32(this.wh_local) : 
                Math::EncodeAsUNorm2(this.bary);
#else
            uint metadata = M_capped;
            uint16_t2 bary_or_wh = Math::EncodeAsUNorm2(this.bary);
#endif
            uint a_x = (uint(bary_or_wh.y) << 16) | bary_or_wh.x;
            uint a_y = (uint(le.y) << 16) | le.x;
            uint a_z = (metadata << 16) | le.z;
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
        bool halfVectorCopyShift;
        BSDF::LOBE lobe;

        float3 target;
        uint lightID;
        float3 lightPos;
        float3 lightNormal;
        bool doubleSided;
        float3 wh_local;
        float partialJacobian;
    };

    bool IsShiftInvertible(Reservoir r_base, BSDF::ShadingData surface_offset, float alpha_min)
    {
#if USE_HALF_VECTOR_COPY_SHIFT == 1
        return !r_base.halfVectorCopyShift ||
            (BSDF::IsLobeValid(surface_offset, r_base.lobe) &&
            (BSDF::LobeAlpha(surface_offset, r_base.lobe) <= alpha_min));
#else
        return true;
#endif
    }
}

#endif