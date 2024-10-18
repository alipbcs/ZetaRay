#ifndef RESTIR_PT_RESERVOIR_H
#define RESTIR_PT_RESERVOIR_H

#include "../IndirectLighting_Common.h"
#include "Shift.hlsli"

namespace RPT_Util
{
    struct Reservoir
    {
        static Reservoir Init()
        {
            Reservoir res;
            res.rc = Reconnection::Init();
            res.w_sum = 0;
            res.W = 0;
            res.M = 0;
            res.target = 0;

            return res;
        }
        
        bool Update(float weight, float3 target, Reconnection rc, inout RNG rng)
        {
            if(isnan(weight))
                return false;

            this.M += 1;

            if(weight == 0)
                return false;

            this.w_sum += weight;

            if (rng.Uniform() < (weight / this.w_sum))
            {
                this.rc.Clear();
                this.rc = rc;
                this.target = target;

                return true;
            }

            return false;
        }

        template<typename TexC, typename TexD, typename TexE>
        void LoadCase1(uint2 DTid, TexC g_inC, TexD g_inD, TexE g_inE)
        {
            uint4 inC = g_inC[DTid];
            uint4 inD = g_inD[DTid];
            half inE = g_inE[DTid];

            this.rc.partialJacobian = asfloat(inC.x);
            this.rc.seed_replay = inC.y;
            this.rc.ID = inC.z;
            this.rc.w_k_lightNormal_w_sky = Math::DecodeOct32(Math::UnpackUintToUint16(inD.z));
            this.rc.x_k = float3(asfloat(inC.w), asfloat(inD.xy));

            uint16_t2 lh = uint16_t2(inD.w & 0xffff, inD.w >> 16);
            this.rc.L = half3(asfloat16(lh), inE);
        }

        template<bool Emissive, typename TexC, typename TexD, typename TexE, typename TexF, typename TexG>
        void LoadCase2(uint2 DTid, TexC g_inC, TexD g_inD, TexE g_inE, TexF g_inF, TexG g_inG)
        {
            uint4 inC = g_inC[DTid];
            uint4 inD = g_inD[DTid];

            this.rc.partialJacobian = asfloat(inC.x);
            this.rc.seed_replay = inC.y;
            this.rc.ID = inC.z;
            this.rc.x_k = float3(asfloat(inC.w), asfloat(inD.xy));

            if(Emissive)
            {
                half inE = g_inE[DTid];
                float2 inF = g_inF[DTid];
                uint inG = g_inG[DTid];

                uint16_t2 lh = uint16_t2(inD.w & 0xffff, inD.w >> 16);
                this.rc.L = half3(asfloat16(lh), inE);
                this.rc.w_k_lightNormal_w_sky = Math::DecodeOct32(Math::UnpackUintToUint16(inD.z));
                this.rc.lightPdf = inF.x;
                this.rc.dwdA = inF.y;
                this.rc.seed_nee = inG;
            }
            else
            {
                if(this.rc.lt_k_plus_1 == Light::TYPE::SKY)
                {
                    this.rc.w_k_lightNormal_w_sky = Math::DecodeOct32(Math::UnpackUintToUint16(inD.z));
                    this.rc.seed_nee = inD.w;
                }
            }
        }

        template<bool Emissive, typename TexC, typename TexD, typename TexE, typename TexF>
        void LoadCase3(uint2 DTid, TexC g_inC, TexD g_inD, TexE g_inE, TexF g_inF)
        {
            uint4 inC = Emissive ? g_inC[DTid] : uint4(g_inC[DTid].xyz, 0);
            uint4 inD = Emissive ? g_inD[DTid] : uint4(0.xx, g_inD[DTid].zw);

            this.rc.seed_replay = inC.y;
            this.rc.ID = inC.z;

            if(Emissive)
            {
                this.rc.partialJacobian = this.rc.lobe_k_min_1 == BSDF::LOBE::ALL ? 
                    1.0f : asfloat(inC.x);

                half inE = g_inE[DTid];
                float inF = g_inF[DTid].x;

                this.rc.x_k = float3(asfloat(inC.w), asfloat(inD.xy));

                uint16_t2 lh = uint16_t2(inD.w & 0xffff, inD.w >> 16);
                this.rc.L = half3(asfloat16(lh), inE);
                this.rc.lightPdf = inF.x;
                this.rc.seed_nee = inC.x;
                // light normal
                this.rc.w_k_lightNormal_w_sky = Math::DecodeOct32(Math::UnpackUintToUint16(inD.z));
            }
            else
            {
                this.rc.partialJacobian = asfloat(inC.x);

                if(this.rc.lt_k == Light::TYPE::SKY)
                {
                    this.rc.w_k_lightNormal_w_sky = Math::DecodeOct32(Math::UnpackUintToUint16(inD.z));
                    this.rc.seed_nee = inD.w;
                }
            }
        }

        void UnpackMetadata(uint3 metadata)
        {
            uint16_t k = uint16_t(metadata.x & 0xf);
            this.rc.k = k == Reconnection::EMPTY ? k : k + (uint16_t)2;
            this.rc.lobe_k_min_1 = BSDF::LobeFromValue(metadata.y & 0x7);
            this.rc.lobe_k = BSDF::LobeFromValue((metadata.y >> 3) & 0x7);
            this.rc.lt_k = Light::TypeFromValue((metadata.y >> 6) & 0x3);
            this.rc.lt_k_plus_1 = Light::TypeFromValue(metadata.z);
            this.M = (uint16_t)(metadata.x >> 4);
        }

        void UnpackMetadataX(uint metadata)
        {
            uint16_t k = uint16_t(metadata.x & 0xf);
            this.rc.k = k == Reconnection::EMPTY ? k : k + (uint16_t)2;
            this.M = (uint16_t)(metadata.x >> 4);
        }

        template<typename TexA>
        static Reservoir Load_Metadata(uint2 DTid, uint inputAIdx)
        {
            TexA g_inA = ResourceDescriptorHeap[inputAIdx];

            Reservoir ret;
            ret.UnpackMetadata(g_inA[DTid].xyz);

            return ret;
        }

        template<typename TexA>
        static Reservoir Load_MetadataX(uint2 DTid, uint inputAIdx)
        {
            TexA g_inA = ResourceDescriptorHeap[inputAIdx];

            Reservoir ret;
            ret.UnpackMetadata(g_inA[DTid].x);

            return ret;
        }

        template<typename TexA, typename TexB>
        static Reservoir Load_NonReconnection(uint2 DTid, uint inputAIdx, uint inputBIdx)
        {
            TexA g_inA = ResourceDescriptorHeap[inputAIdx];
            TexB g_inB = ResourceDescriptorHeap[inputBIdx];

            Reservoir ret;
            ret.UnpackMetadata(g_inA[DTid].xyz);

            float2 inB = g_inB[DTid];
            ret.w_sum = inB.x;
            ret.W = inB.y;

            return ret;
        }

        template<bool Emissive, typename TexC, typename TexD, typename TexE, typename TexF, typename TexG>
        void Load_Reconnection(uint2 DTid, uint inputCIdx, uint inputDIdx, uint inputEIdx, 
            uint inputFIdx, uint inputGIdx)
        {
            TexC g_inC = ResourceDescriptorHeap[inputCIdx];
            TexD g_inD = ResourceDescriptorHeap[inputDIdx];
            TexE g_inE = ResourceDescriptorHeap[inputEIdx];
            TexF g_inF = ResourceDescriptorHeap[inputFIdx];
            TexG g_inG = ResourceDescriptorHeap[inputGIdx];

            if(this.rc.IsCase1())
                this.LoadCase1<TexC, TexD, TexE>(DTid, g_inC, g_inD, g_inE);
            else if(this.rc.IsCase2())
                this.LoadCase2<Emissive, TexC, TexD, TexE, TexF, TexG>(DTid, g_inC, g_inD, g_inE, g_inF, g_inG);
            else
                this.LoadCase3<Emissive, TexC, TexD, TexE, TexF>(DTid, g_inC, g_inD, g_inE, g_inF);
        }

        template<bool Emissive, typename TexA, typename TexB, typename TexC, typename TexD, 
            typename TexE, typename TexF, typename TexG>
        static Reservoir Load(uint2 DTid, uint inputAIdx, uint inputBIdx, uint inputCIdx, 
            uint inputDIdx, uint inputEIdx, uint inputFIdx, uint inputGIdx)
        {
            TexA g_inA = ResourceDescriptorHeap[inputAIdx];
            TexB g_inB = ResourceDescriptorHeap[inputBIdx];
            TexC g_inC = ResourceDescriptorHeap[inputCIdx];
            TexD g_inD = ResourceDescriptorHeap[inputDIdx];
            TexE g_inE = ResourceDescriptorHeap[inputEIdx];
            TexF g_inF = ResourceDescriptorHeap[inputFIdx];
            TexG g_inG = ResourceDescriptorHeap[inputGIdx];

            Reservoir ret;
            ret.UnpackMetadata(g_inA[DTid].xyz);

            float2 inB = g_inB[DTid];
            ret.w_sum = inB.x;
            ret.W = inB.y;

            if(ret.rc.Empty())
                return ret;
  
            if(ret.rc.IsCase1())
                ret.LoadCase1<TexC, TexD, TexE >(DTid, g_inC, g_inD, g_inE);
            else if(ret.rc.IsCase2())
                ret.LoadCase2<Emissive, TexC, TexD, TexE, TexF, TexG >(DTid, g_inC, g_inD, g_inE, g_inF, g_inG);
            else
                ret.LoadCase3<Emissive, TexC, TexD, TexE, TexF >(DTid, g_inC, g_inD, g_inE, g_inF);

            return ret;
        }

        void LoadTarget(uint2 DTid, uint uavIdx)
        {
            RWTexture2D<float4> g_target = ResourceDescriptorHeap[uavIdx];
            this.target = g_target[DTid].xyz;
        }

        void LoadWSum(uint2 DTid, uint inputBIdx)
        {
            RWTexture2D<float2> g_inB = ResourceDescriptorHeap[inputBIdx];
            this.w_sum = g_inB[DTid].x;
        }

        static float LoadW(uint2 DTid, uint inputBIdx)
        {
            RWTexture2D<float2> g_inB = ResourceDescriptorHeap[inputBIdx];
            return g_inB[DTid].y;
        }

        void WriteCase1(uint2 DTid, RWTexture2D<uint4> g_outC, RWTexture2D<uint4> g_outD, 
            RWTexture2D<half> g_outE)
        {
            g_outC[DTid] = uint4(asuint(this.rc.partialJacobian), this.rc.seed_replay, 
                this.rc.ID, asuint(this.rc.x_k.x));

            uint16_t2 temp = Math::EncodeOct32(this.rc.w_k_lightNormal_w_sky);
            uint w_k_encoded = temp.x | (uint(temp.y) << 16);

            half2 lh = this.rc.L.rg;
            g_outD[DTid] = uint4(asuint(this.rc.x_k.yz), w_k_encoded, 
                asuint16(lh.r) | (uint(asuint16(lh.g)) << 16));
            g_outE[DTid] = this.rc.L.b;
        }

        template<bool Emissive>
        void WriteCase2(uint2 DTid, RWTexture2D<uint4> g_outC, RWTexture2D<uint4> g_outD, 
            RWTexture2D<half> g_outE, RWTexture2D<float2> g_outF, RWTexture2D<uint> g_outG)
        {
            g_outC[DTid] = uint4(asuint(this.rc.partialJacobian), this.rc.seed_replay, 
                this.rc.ID, asuint(this.rc.x_k.x));

            uint16_t2 temp = Math::EncodeOct32(this.rc.w_k_lightNormal_w_sky);
            uint w_k_encoded = temp.x | (uint(temp.y) << 16);

            if(Emissive)
            {
                half2 lh = this.rc.L.rg;
                g_outD[DTid] = uint4(asuint(this.rc.x_k.yz), w_k_encoded, 
                    asuint16(lh.r) | (uint(asuint16(lh.g)) << 16));
                g_outE[DTid] = this.rc.L.b;
                g_outF[DTid] = float2(this.rc.lightPdf, this.rc.dwdA);
                g_outG[DTid] = this.rc.seed_nee;
            }
            else
            {
                if(this.rc.lt_k_plus_1 == Light::TYPE::SKY)
                    g_outD[DTid] = uint4(asuint(this.rc.x_k.yz), w_k_encoded, this.rc.seed_nee);
                else
                    g_outD[DTid].xy = asuint(this.rc.x_k.yz);
            }
        }

        template<bool Emissive>
        void WriteCase3(uint2 DTid, RWTexture2D<uint4> g_outC, RWTexture2D<uint4> g_outD, 
            RWTexture2D<half> g_outE, RWTexture2D<float2> g_outF)
        {
            // w_sky or light normal
            uint16_t2 temp = Math::EncodeOct32(this.rc.w_k_lightNormal_w_sky);
            uint w_k_encoded = temp.x | (uint(temp.y) << 16);

            if(Emissive)
            {
                // NEE seed is only needed for light sampling, jacobian is only
                // needed for BSDF sampling
                uint v = this.rc.lobe_k_min_1 == BSDF::LOBE::ALL ? this.rc.seed_nee :
                    asuint(this.rc.partialJacobian);

                g_outC[DTid] = uint4(v, this.rc.seed_replay, this.rc.ID, asuint(this.rc.x_k.x));

                half2 lh = this.rc.L.rg;
                g_outD[DTid] = uint4(asuint(this.rc.x_k.yz), w_k_encoded, 
                    asuint16(lh.r) | uint(asuint16(lh.g)) << 16);
                g_outE[DTid] = this.rc.L.b;
                g_outF[DTid].x = this.rc.lightPdf;
            }
            else
            {
                g_outC[DTid].xyz = uint3(asuint(this.rc.partialJacobian), this.rc.seed_replay, this.rc.ID);

                if(this.rc.lt_k == Light::TYPE::SKY)
                    g_outD[DTid].zw = uint2(w_k_encoded, this.rc.seed_nee);
            }
        }

        void WriteReservoirData(uint2 DTid, uint outputAIdx, uint outputBIdx, uint M_max)
        {
            RWTexture2D<uint4> g_outA = ResourceDescriptorHeap[outputAIdx];
            RWTexture2D<float2> g_outB = ResourceDescriptorHeap[outputBIdx];

            uint k = this.rc.Empty() ? this.rc.k : max(this.rc.k, 2) - 2;
            uint m = min(this.M, M_max);

            g_outA[DTid].x = k | (m << 4);
            g_outB[DTid] = float2(this.w_sum, this.W);
        }

        // Skips w_sum
        void WriteReservoirData2(uint2 DTid, uint outputAIdx, uint outputBIdx, uint M_max)
        {
            RWTexture2D<uint4> g_outA = ResourceDescriptorHeap[outputAIdx];
            RWTexture2D<float2> g_outB = ResourceDescriptorHeap[outputBIdx];

            uint k = this.rc.Empty() ? this.rc.k : max(this.rc.k, 2) - 2;
            uint m = min(this.M, M_max);

            g_outA[DTid].x = k | (m << 4);
            g_outB[DTid].y = this.W;
        }

        void WriteWSum(uint2 DTid, uint outputBIdx)
        {
            RWTexture2D<float2> g_outB = ResourceDescriptorHeap[outputBIdx];
            g_outB[DTid].x = this.w_sum;
        }

        void WriteTarget(uint2 DTid, uint uavIdx)
        {
            RWTexture2D<float4> g_target = ResourceDescriptorHeap[uavIdx];
            this.target = Math::Sanitize(this.target);
            g_target[DTid].xyz = this.target;
        }

        template<bool Emissive>
        void Write(uint2 DTid, uint outputAIdx, uint outputBIdx, uint outputCIdx, 
            uint outputDIdx, uint outputEIdx, uint outputFIdx, uint outputGIdx, 
            uint M_max = 0)
        {
            // (metadata)
            RWTexture2D<uint4> g_outA = ResourceDescriptorHeap[outputAIdx];
            // (w_sum, W)
            RWTexture2D<float2> g_outB = ResourceDescriptorHeap[outputBIdx];
            // (jacobian, seed_replay, ID, x_k.x)
            RWTexture2D<uint4> g_outC = ResourceDescriptorHeap[outputCIdx];
            // (x_k.yz, w_k/w_sky/lightNormal, L.rg | seed_nee)
            RWTexture2D<uint4> g_outD = ResourceDescriptorHeap[outputDIdx];
            // (L.b)
            RWTexture2D<half> g_outE = ResourceDescriptorHeap[outputEIdx];
            // (lightPdf, dwdA)
            RWTexture2D<float2> g_outF = ResourceDescriptorHeap[outputFIdx];
            // (seed_nee)
            RWTexture2D<uint> g_outG = ResourceDescriptorHeap[outputGIdx];

            // A
            uint m = M_max == 0 ? this.M : min(this.M, M_max);
            uint k = this.rc.Empty() ? this.rc.k : max(this.rc.k, 2) - 2;

            uint3 metadata;
            metadata.x = k | (m << 4);
            metadata.y = BSDF::LobeToValue(this.rc.lobe_k_min_1) |
                (BSDF::LobeToValue(this.rc.lobe_k) << 3) |
                (Light::TypeToValue(this.rc.lt_k) << 6);
            metadata.z = Light::TypeToValue(this.rc.lt_k_plus_1);
            g_outA[DTid].xyz = metadata;

            // B
            this.w_sum = Math::Sanitize(this.w_sum);
            this.W = Math::Sanitize(this.W);
            g_outB[DTid] = float2(this.w_sum, this.W);

            if(this.rc.Empty())
                return;

            // All cases:
            //  - metadata
            //  - Jacobian
            //  - seed_replay

            // Case 1) x_k and x_{k + 1} not on a light source
            //  - x_k
            //  - ID
            //  - L
            //  - w_k_lightNormal_w_sky (= w_k)
            if(this.rc.IsCase1())
                this.WriteCase1(DTid, g_outC, g_outD, g_outE);

            // Case 2) x_{k + 1} on a light source
            //  - x_k
            //  - ID
            //  - lightPdf: if x_{k + 1} is emissive
            //  - rng_nee: if x_{k + 1} is on sky
            //  - L: if x_{k + 1} is emissive
            //  - w_k_lightNormal_w_sky: if x_{k + 1} is sky (= w_sky) or emissive (= w_k)
            else if(this.rc.IsCase2())
                this.WriteCase2<Emissive>(DTid, g_outC, g_outD, g_outE, g_outF, g_outG);

            // Case 3) x_k on a light source
            //  - x_k: if on emissive
            //  - ID: if on emissive (= lightID)
            //  - lightPdf: if x_k is emissive
            //  - rng_nee: if x_k is on sky
            //  - L: if x_k is emissive
            //  - w_k_lightNormal_w_sky: if x_k on sky (= w_sky) or is emissive (= lightNormal)
            else
                this.WriteCase3<Emissive>(DTid, g_outC, g_outD, g_outE, g_outF);
        }

        float w_sum;
        float W;
        float3 target;
        Reconnection rc;
        uint16_t M;
    };
}

#endif