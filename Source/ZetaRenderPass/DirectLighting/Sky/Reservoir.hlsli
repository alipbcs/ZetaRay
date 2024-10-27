#ifndef SKY_DI_RESERVOIR_H
#define SKY_DI_RESERVOIR_H

#include "SkyDI_Common.h"
#include "Util.hlsli"

namespace SkyDI_Util
{
    struct Reservoir
    {
        static Reservoir Init()
        {
            Reservoir res;
            res.M = 0;
            res.w_sum = 0;
            res.W = 0;
            res.wx = 0.0;
            res.target = 0;
            res.lightType = Light::TYPE::NONE;
            res.partialJacobian = 1;
            res.halfVectorCopyShift = false;
            res.lobe = BSDF::LOBE::ALL;

            return res;
        }

        static Reservoir Load(uint2 DTid, uint inputAIdx, uint inputBIdx, uint inputCIdx)
        {
            Texture2D<uint> g_reservoir_A = ResourceDescriptorHeap[inputAIdx];
            Texture2D<uint2> g_reservoir_B = ResourceDescriptorHeap[inputBIdx];
            Texture2D<float2> g_reservoir_C = ResourceDescriptorHeap[inputCIdx];

            Reservoir ret = Reservoir::Init();
            const uint metadata = g_reservoir_A[DTid];
            ret.M = (uint16)(metadata & 0xf);

            bool wSumGt0 = (metadata >> 7) > 0;
            if(!wSumGt0)
                return ret;

            ret.lightType = (metadata >> 4) & 0x1 ? Light::TYPE::SKY : Light::TYPE::SUN;
            ret.halfVectorCopyShift = (metadata >> 5) & 0x1;
            bool lobeIsCoat = (metadata >> 6) & 0x1;
            if(ret.halfVectorCopyShift)
                ret.lobe = lobeIsCoat ? BSDF::LOBE::COAT : BSDF::LOBE::GLOSSY_R;
            else
                ret.lobe = BSDF::LOBE::ALL;

            uint16_t2 temp = (uint16_t2)g_reservoir_B[DTid];
            ret.wx = Math::DecodeOct32(temp);

            const float2 weights = g_reservoir_C[DTid];
            ret.w_sum = weights.x;
            ret.W = weights.y;

            return ret;
        }

        void LoadTarget(uint2 DTid, uint uavIdx)
        {
            RWTexture2D<float4> g_target = ResourceDescriptorHeap[uavIdx];
            this.target = g_target[DTid].xyz;
        }

        bool IsValid()
        {
            // return dot(this.wx, this.wx) != 0;
            return this.w_sum > 0;
        }

        bool Update(float weight, float3 wi, float3 wo, float3 normal, Light::TYPE lt, BSDF::LOBE lb, 
            bool halfVecShift, float3 target, inout RNG rng)
        {
            if(isnan(weight))
                return false;

            this.M += 1;

            if(weight == 0)
                return false;

            this.w_sum += weight;

            if (rng.Uniform() < (weight / this.w_sum))
            {
                this.target = target;
                this.lightType = lt;
                this.lobe = lb;
                this.halfVectorCopyShift = halfVecShift;

                // Sample uses half-vector copy shift
                if(halfVecShift)
                {
                    float3 wh = normalize(wo + wi);
                    float3 wh_local = WorldToTangentFrame(normal, wh);

                    this.wx = wh_local;
                    this.partialJacobian = abs(dot(wh, wo));
                }
                else
                    this.wx = wi;

                return true;
            }

            return false;
        }

        bool Update(float weight, float3 wi_or_wh, Light::TYPE lt, BSDF::LOBE lb, 
            bool halfVecShift, float whdotwo, float3 target, inout RNG rng)
        {
            if(isnan(weight))
                return false;

            this.M += 1;

            if(weight == 0)
                return false;

            this.w_sum += weight;

            if (rng.Uniform() < (weight / this.w_sum))
            {
                this.wx = wi_or_wh;
                this.target = target;
                this.lightType = lt;
                this.lobe = lb;
                this.halfVectorCopyShift = halfVecShift;
                this.partialJacobian = whdotwo;

                return true;
            }

            return false;
        }

        void WriteTarget(uint2 DTid, uint uavIdx)
        {
            RWTexture2D<float4> g_target = ResourceDescriptorHeap[uavIdx];
            this.target = Math::Sanitize(this.target);
            g_target[DTid].xyz = this.target;
        }

        void Write(uint2 DTid, uint outputAIdx, uint outputBIdx, uint outputCIdx, 
            uint M_max)
        {
            RWTexture2D<uint> g_outReservoir_A = ResourceDescriptorHeap[outputAIdx];
            RWTexture2D<uint2> g_outReservoir_B = ResourceDescriptorHeap[outputBIdx];
            RWTexture2D<float2> g_outReservoir_C = ResourceDescriptorHeap[outputCIdx];

            // metadata
            uint M_capped = min(this.M, M_max) & 0xf;
            bool wSumGt0 = this.w_sum > 0;
            uint metadata = M_capped | 
                (uint(this.lightType == Light::TYPE::SKY) << 4) |
                (uint(this.halfVectorCopyShift) << 5) |
                (uint(this.lobe == BSDF::LOBE::COAT) << 6) |
                (uint(wSumGt0) << 7);
            g_outReservoir_A[DTid] = metadata;

            if(!wSumGt0)
                return;

            g_outReservoir_B[DTid] = Math::EncodeOct32(this.wx);
            g_outReservoir_C[DTid] = float2(this.w_sum, this.W);
        }

        float w_sum;
        float W;
        // Either wi or wh
        float3 wx;
        float3 target;
        float partialJacobian;
        bool halfVectorCopyShift;
        uint16 M;
        BSDF::LOBE lobe;
        Light::TYPE lightType;
    };

    bool IsShiftInvertible(Reservoir r_base, BSDF::ShadingData surface_offset, float alpha_min)
    {
        return !r_base.halfVectorCopyShift ||
            (BSDF::IsLobeValid(surface_offset, r_base.lobe) &&
            (BSDF::LobeAlpha(surface_offset, r_base.lobe) <= alpha_min));
    }
}

#endif