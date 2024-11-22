#ifndef RESTIR_PT_SHIFT_H
#define RESTIR_PT_SHIFT_H

#include "ReSTIR_PT_NEE.hlsli"

namespace RPT_Util
{
    enum SHIFT_ERROR : uint16_t
    {
        SUCCESS = 0,
        INVALID_PIXEL = 1,
        NOT_FOUND = 2,
        EMPTY = 4
    };

    struct Reconnection
    {
        static Reconnection Init()
        {
            Reconnection ret;
            ret.k = Reconnection::EMPTY;
            ret.lt_k = Light::TYPE::NONE;
            ret.lt_k_plus_1 = Light::TYPE::NONE;
            ret.partialJacobian = 0;
            ret.x_k = FLT_MAX;
            ret.seed_replay = 0;
            ret.w_k_lightNormal_w_sky = 0;
            ret.L = 0;
            ret.lightPdf = 0;
            ret.seed_nee = 0;
            ret.dwdA = 0;

            return ret;
        }

        bool Empty()
        {
            return this.k == Reconnection::EMPTY;
        }

        bool IsCase1()
        {
            return !this.IsCase2() && !this.IsCase3();
        }

        bool IsCase2()
        {
            return this.lt_k_plus_1 != Light::TYPE::NONE;
        }

        bool IsCase3()
        {
            return this.lt_k != Light::TYPE::NONE;
        }

        void Clear()
        {
            this.k = Reconnection::EMPTY;
            this.lt_k = Light::TYPE::NONE;
            this.lt_k_plus_1 = Light::TYPE::NONE;
        }

        // w_{k - 1} = \hat {x_k - x_{k - 1}}
        // t = || x_k - x_{k - 1} ||
        void SetCase1(int16 k, float3 x_k, float t, float3 normal_k, uint hitID, 
            float3 w_k_min_1, BSDF::LOBE l_k_min_1, float pdf_w_k_min_1, 
            float3 w_k, BSDF::LOBE l_k, float pdf_w_k)
        {
            this.lobe_k_min_1 = l_k_min_1;

            this.k = k;
            this.x_k = x_k;
            this.ID = hitID;
            this.lt_k = Light::TYPE::NONE;
            this.lobe_k = l_k;
            this.w_k_lightNormal_w_sky = w_k;

            this.lt_k_plus_1 = Light::TYPE::NONE;

            this.partialJacobian = pdf_w_k_min_1;
            float cos_theta_k = abs(dot(-w_k_min_1, normal_k));
            this.partialJacobian *= cos_theta_k / (t * t);
            this.partialJacobian *= pdf_w_k;
        }

        // seed: RNG state for direct lighting at x_k
        void SetCase2(int16 k, float3 x_k, float t, float3 normal_k, uint hitID, 
            float3 w_k_min_1, BSDF::LOBE l_k_min_1, float pdf_w_k_min_1, 
            float3 w_k, BSDF::LOBE l_k, float pdf_w_k,
            Light::TYPE t_k_plus_1, float pdf_light, float3 le, uint seed, float dwdA)
        {
            this.lobe_k_min_1 = l_k_min_1;

            this.k = k;
            this.x_k = x_k;
            this.ID = hitID;
            this.lt_k = Light::TYPE::NONE;
            this.lobe_k = l_k;
            this.w_k_lightNormal_w_sky = w_k;

            this.lt_k_plus_1 = t_k_plus_1;
            // [Emissives] Cache light pdf and dwdA to avoid recomputing them
            this.lightPdf = pdf_light;
            this.dwdA = dwdA;
            this.seed_nee = seed;
            this.L = half3(le);

            this.partialJacobian = pdf_w_k_min_1;
            float cos_theta_k = abs(dot(-w_k_min_1, normal_k));
            this.partialJacobian *= cos_theta_k / (t * t);

            // Not needed as it cancels out with the same term in offset path
            if(this.lobe_k != BSDF::LOBE::ALL)
                this.partialJacobian *= pdf_w_k;
        }

        // seed: RNG state for direct lighting at x_{k - 1}
        void SetCase3(int16 k, float3 x_k, Light::TYPE t, BSDF::LOBE l_k_min_1, 
            uint lightID, float3 le, float3 lightNormal, float pdf_solidAngle, 
            float pdf_light, float dwdA, float3 w_sky, bool twoSided, uint seed)
        {
            this.lobe_k_min_1 = l_k_min_1;

            this.k = k;
            this.x_k = x_k;
            this.ID = lightID;
            this.lt_k = t;
            this.seed_nee = seed;
            // Jacobian simplifies to 1.0 when x_k was sampled using light sampling
            this.partialJacobian = l_k_min_1 == BSDF::LOBE::ALL ? 
                1.0f :
                pdf_solidAngle * dwdA;
            // [Emissives] Cache light pdf and le to avoid recomputing them
            this.lightPdf = twoSided ? pdf_light : -pdf_light;
            this.L = half3(le);

            this.lt_k_plus_1 = Light::TYPE::NONE;

            if(t == Light::TYPE::EMISSIVE)
                this.w_k_lightNormal_w_sky = lightNormal;
            else if(t == Light::TYPE::SKY)
                this.w_k_lightNormal_w_sky = w_sky;
        }

        static const uint16_t EMPTY = 0xf;

        float3 x_k;
        // Tri ID when x_k is not on a light source and light ID otherwise
        uint ID;
        float partialJacobian;
        // A union =
        //  - w_k, when case = 1 
        //  - w_sky, when case = 2 and lt_{k + 1} = sky or case = 3 and lt_k = sky
        //  - w_light, when case = 2 and lt_{k + 1} = emissive
        //  - light normal, when case = 3
        float3 w_k_lightNormal_w_sky;
        // Cache the light sampling pdf when case = 2 or case = 3
        float lightPdf;
        uint seed_replay;
        uint seed_nee;
        float dwdA;
        half3 L;
        uint16_t k;
        BSDF::LOBE lobe_k_min_1;
        BSDF::LOBE lobe_k;
        Light::TYPE lt_k;
        Light::TYPE lt_k_plus_1;
    };

    struct OffsetPath
    {
        static OffsetPath Init()
        {
            OffsetPath ret;
            ret.target = 0;
            ret.partialJacobian = 0;

            return ret;
        }

        float3 target;
        float partialJacobian;
    };

    struct OffsetPathContext
    {
        static OffsetPathContext Init()
        {
            OffsetPathContext ret;
            ret.throughput = 0;
            ret.pos = 0;
            ret.normal = 0;
            ret.rd = RT::RayDifferentials::Init();
            ret.surface = BSDF::ShadingData::Init();
            ret.eta_curr = ETA_AIR;
            ret.eta_next = DEFAULT_ETA_MAT;
            ret.rngReplay.State = 0;
            // ret.anyGlossyBounces = false;

            return ret;
        }
        
        static OffsetPathContext Load(uint2 DTid, uint srvAIdx, uint srvBIdx, uint srvCIdx, 
            uint srvDIdx, bool isCase3)
        {
            OffsetPathContext ctx = OffsetPathContext::Init();

            Texture2D<float4> g_cbA = ResourceDescriptorHeap[srvAIdx];
            Texture2D<uint4> g_cbB = ResourceDescriptorHeap[srvBIdx];
            Texture2D<uint4> g_cbC = ResourceDescriptorHeap[srvCIdx];
            Texture2D<uint> g_cbD = ResourceDescriptorHeap[srvDIdx];

            float4 inA = 0;

            // Texture filtering is not needed for case 3
            if(!isCase3)
            {
                inA = g_cbA[DTid];
                ctx.rd.uv_grads.xy = inA.w;
                ctx.rd.uv_grads.zw = inA.w;
            }
            else
                inA.xyz = g_cbA[DTid].xyz;

            ctx.throughput = inA.xyz;
            if(dot(ctx.throughput, ctx.throughput) == 0)
                return ctx;

            uint4 inB = g_cbB[DTid];
            uint3 inC = g_cbC[DTid].xyz;

            ctx.pos = asfloat(inB.xyz);
            ctx.normal = Math::DecodeOct32(Math::UnpackUintToUint16(inB.w));
            ctx.eta_curr = mad(Math::UNorm8ToFloat((inC.z >> 8) & 0xff), 1.5f, 1.0f);
            ctx.eta_next = mad(Math::UNorm8ToFloat((inC.z >> 16) & 0xff), 1.5f, 1.0f);

            float3 wo = Math::DecodeOct32(Math::UnpackUintToUint16(inC.x));
            float roughness = Math::UNorm8ToFloat(inC.z & 0xff);
            float3 baseColor = Math::UnpackRGB8(inC.y & 0xffffff);
            uint flags = inC.y >> 24;
            bool metallic = flags & 0x1;
            // ctx.anyGlossyBounces = (flags & 0x2) == 0x2;
            bool specTr = (flags & 0x4) == 0x4;
            half trDepth = (flags & 0x8) == 0x8 ? 1.0h : 0.0h;
            bool coated = (flags & 0x10) == 0x10;
            float subsurface = Math::UNorm8ToFloat((inC.z >> 24) & 0xff);

            // UNorm8 encoding above preserves ETA_AIR = 1, so the comparison
            // below works
            float eta_next = ctx.eta_curr == ETA_AIR ? ctx.eta_next : ETA_AIR;

            float coat_weight = 0;
            float3 coat_color = 0.0f;
            float coat_roughness = 0;
            float coat_ior = DEFAULT_ETA_COAT;

            if(coated)
            {
                uint c_w = g_cbC[DTid].w;
                uint d_w = g_cbD[DTid];

                coat_weight = Math::UNorm8ToFloat((c_w >> 24) & 0xff);
                coat_color = Math::UnpackRGB8(c_w & 0xffffff);
                coat_roughness = Math::UNorm8ToFloat(d_w & 0xff);
                coat_ior = mad(Math::UNorm8ToFloat((d_w >> 8) & 0xff), 1.5f, 1.0f);
            }

            ctx.surface = BSDF::ShadingData::Init(ctx.normal, wo, metallic, roughness, 
                baseColor, ctx.eta_curr, eta_next, specTr, trDepth, (half)subsurface,
                coat_weight, coat_color, coat_roughness, coat_ior);

            return ctx;
        }

        void Write(uint2 DTid, uint uavAIdx, uint uavBIdx, uint uavCIdx, uint uavDIdx,
            bool isCase3)
        {
            // R16G16B16A16_FLOAT
            RWTexture2D<float4> g_rbA = ResourceDescriptorHeap[uavAIdx];
            // R32G32B32A32_UINT
            RWTexture2D<uint4> g_rbB = ResourceDescriptorHeap[uavBIdx];
            // R32G32B32A32_UINT
            RWTexture2D<uint4> g_rbC = ResourceDescriptorHeap[uavCIdx];
            // R16_UINT
            RWTexture2D<uint> g_rbD = ResourceDescriptorHeap[uavDIdx];

            // Texture filtering is not needed for case 3
            if(!isCase3)
            {
                float ddx_uv = sqrt(dot(this.rd.uv_grads.xy, this.rd.uv_grads.xy));
                float ddy_uv = sqrt(dot(this.rd.uv_grads.zw, this.rd.uv_grads.zw));
                float grad_max = max(ddx_uv, ddy_uv);

                g_rbA[DTid] = float4(this.throughput, grad_max);
            }
            else
                g_rbA[DTid].xyz = this.throughput;

            if(dot(this.throughput, this.throughput) == 0)
                return;

            uint16_t2 e1 = Math::EncodeOct32(this.normal);
            uint normal_e = e1.x | (uint(e1.y) << 16);

            // ShadingData
            uint16_t2 e2 = Math::EncodeOct32(this.surface.wo);
            uint wo = e2.x | (uint(e2.y) << 16);
            // Note: needed so that BSDF evaluation at x_{k - 1} uses the right
            // transmission tint
            bool hasVolumetricInterior = surface.trDepth > 0;
            uint flags = (uint)surface.metallic | 
                // ((uint)this.anyGlossyBounces << 1) | 
                ((uint)surface.specTr << 2) |
                ((uint)hasVolumetricInterior << 3) |
                ((uint)surface.Coated() << 4);
            uint baseColor_Flags = Math::Float3ToRGB8(surface.baseColor_Fr0_TrCol);
            baseColor_Flags = baseColor_Flags | (flags << 24);
            uint roughness = Math::FloatToUNorm8(!surface.GlossSpecular() ? sqrt(surface.alpha) : 0);
            uint eta_curr = Math::FloatToUNorm8((this.eta_curr - 1.0f) / 1.5f);
            uint eta_next = Math::FloatToUNorm8((this.eta_next - 1.0f) / 1.5f);
            uint subsurface = Math::FloatToUNorm8((float)surface.subsurface);
            uint packed = roughness | (eta_curr << 8) | (eta_next << 16) | (subsurface << 24);

            g_rbB[DTid] = uint4(asuint(this.pos), normal_e);

            if(surface.Coated())
            {
                uint coat_weight = Math::FloatToUNorm8(surface.coat_weight);
                uint coat_color = Math::Float3ToRGB8(surface.coat_color);
                uint coat_roughness = Math::FloatToUNorm8(!surface.CoatSpecular() ? 
                    sqrt(surface.coat_alpha) : 0);
                float coat_eta = surface.coat_eta >= 1.0f ? surface.coat_eta : 
                    1.0f / surface.coat_eta;
                uint coat_eta_e = Math::FloatToUNorm8((coat_eta - 1.0f) / 1.5f);

                g_rbC[DTid] = uint4(wo, baseColor_Flags, packed, coat_color | (coat_weight << 24));
                g_rbD[DTid] = coat_roughness | (coat_eta_e << 8);
            }
            else
                g_rbC[DTid].xyz = uint3(wo, baseColor_Flags, packed);
        }

        float3 throughput;
        float3 pos;
        float3 normal;
        RT::RayDifferentials rd;
        BSDF::ShadingData surface;
        float eta_curr;
        float eta_next;
        RNG rngReplay;
        // bool anyGlossyBounces;
    };

    bool CanReconnect(float alpha_lobe_k_min_1, float alpha_lobe_k, float3 x_k_min_1, float3 x_k, 
        BSDF::LOBE lobe_k_min_1, BSDF::LOBE lobe_k, float alpha_min)
    {
        if((alpha_lobe_k_min_1 < alpha_min) || (alpha_lobe_k < alpha_min))
            return false;

        if((lobe_k_min_1 == BSDF::LOBE::GLOSSY_T) && (lobe_k == BSDF::LOBE::GLOSSY_T))
            return false;

        // float3 delta = x_k - x_k_min_1;
        // float distSq = dot(delta, delta);
        // if(distSq < d_min)
        //     return false;

        return true;
    }

    void Replay(int16 numBounces, BSDF::BSDFSample bsdfSample, float alpha_min, 
        bool regularization, ConstantBuffer<cbFrameConstants> g_frame, SamplerState samp,
        ReSTIR_Util::Globals globals, inout OffsetPathContext ctx)
    {
        ctx.throughput = bsdfSample.bsdfOverPdf;
        int bounce = 0;
        ctx.eta_curr = dot(ctx.normal, bsdfSample.wi) < 0 ? ctx.eta_next : ETA_AIR;
        bool inTranslucentMedium = ctx.eta_curr != ETA_AIR;
        // Note: skip the first bounce for a milder impacet. May have to change in the future.
        // ctx.anyGlossyBounces = bsdfSample.lobe != BSDF::LOBE::DIFFUSE_R;
        // ctx.anyGlossyBounces = false;

        float alpha_lobe_prev = BSDF::LobeAlpha(ctx.surface, bsdfSample.lobe);
        bool tr_prev = ctx.surface.specTr;
        BSDF::LOBE lobe_prev = bsdfSample.lobe;
        float3 pos_prev = ctx.pos;

        while(true)
        {
            RtRayQuery::Hit hitInfo = RtRayQuery::Hit::FindClosest<false>(ctx.pos, ctx.normal, 
                bsdfSample.wi, globals.bvh, globals.frameMeshData, globals.vertices, 
                globals.indices, ctx.surface.Transmissive());

            // Not invertible -- base path would've stopped here
            if(!hitInfo.hit)
            {
                ctx.throughput = 0;
                return;
            }

            float3 newPos = mad(hitInfo.t, bsdfSample.wi, ctx.pos);
            float3 dpdx;
            float3 dpdy;
            ctx.rd.dpdx_dpdy(newPos, hitInfo.normal, dpdx, dpdy);
            ctx.rd.ComputeUVDifferentials(dpdx, dpdy, hitInfo.triDiffs.dpdu, hitInfo.triDiffs.dpdv);

            if(!RtRayQuery::GetMaterialData(-bsdfSample.wi, globals.materials, g_frame, ctx.eta_curr, 
                ctx.rd.uv_grads, hitInfo, ctx.surface, ctx.eta_next, samp))
            {
                // Not invertible
                ctx.throughput = 0;
                return;
            }

            // Make sure these are updated before breaking
            ctx.pos = mad(hitInfo.t, bsdfSample.wi, ctx.pos);
            ctx.normal = hitInfo.normal;
            bounce++;

            // if(regularization && ctx.anyGlossyBounces)
            //     ctx.surface.Regularize();

            if(inTranslucentMedium && (ctx.surface.trDepth > 0))
            {
                float3 extCoeff = -log(ctx.surface.baseColor_Fr0_TrCol) / ctx.surface.trDepth;
                ctx.throughput *= exp(-hitInfo.t * extCoeff);
            }

            // Remaining code can be skipped in last iteration
            if(bounce >= numBounces)
                break;

            // Sample BSDF to generate new direction
            bsdfSample = BSDF::SampleBSDF(ctx.normal, ctx.surface, ctx.rngReplay);

            // Not invertible -- base path would've stopped here
            if(dot(bsdfSample.bsdfOverPdf, bsdfSample.bsdfOverPdf) == 0)
            {
                ctx.throughput = 0;
                return;
            }

            const float alpha_lobe = BSDF::LobeAlpha(ctx.surface, bsdfSample.lobe);

            // Not invertible -- reconnection vertex must match
            if(RPT_Util::CanReconnect(alpha_lobe_prev, alpha_lobe, pos_prev, ctx.pos, lobe_prev, 
                bsdfSample.lobe, alpha_min))
            {
                ctx.throughput = 0;
                return;
            }

            const bool transmitted = dot(ctx.normal, bsdfSample.wi) < 0;
            ctx.eta_curr = transmitted ? (ctx.eta_curr == ETA_AIR ? ctx.eta_next : ETA_AIR) : ctx.eta_curr;
            ctx.throughput *= bsdfSample.bsdfOverPdf;
            // ctx.anyGlossyBounces = ctx.anyGlossyBounces || (bsdfSample.lobe != BSDF::LOBE::DIFFUSE_R);
            inTranslucentMedium = ctx.eta_curr != ETA_AIR;

            pos_prev = ctx.pos;
            alpha_lobe_prev = alpha_lobe;
            tr_prev = ctx.surface.specTr;
            lobe_prev = bsdfSample.lobe;

            ctx.rd.UpdateRays(ctx.pos, ctx.normal, bsdfSample.wi, ctx.surface.wo, 
                hitInfo.triDiffs, dpdx, dpdy, transmitted, ctx.surface.eta);
        }
    }

    // Inputs:
    //  - beta: Throughput of path y_1, ... y_{k - 2}
    //  - surface: Shading data at vertex y_{k - 1}
    //  - eta_next: IOR of material at vertex y_{k - 1}
    template<bool Emissive>
    OffsetPath Reconnect_Case1Or2(float3 beta, float3 y_k_min_1, float3 normal_k_min_1, 
        float eta_curr, float eta_next, RT::RayDifferentials rd, BSDF::ShadingData surface, 
        bool anyGlossyBounces, float alpha_min, bool regularization, Reconnection rc, 
        ConstantBuffer<cbFrameConstants> g_frame, ReSTIR_Util::Globals globals, 
        RNG rngReplay, RNG rngNEE)
    {
        OffsetPath ret = OffsetPath::Init();

        // Not invertible
        if(!BSDF::IsLobeValid(surface, rc.lobe_k_min_1))
            return ret;

        float alpha_lobe_k_min_1 = BSDF::LobeAlpha(surface, rc.lobe_k_min_1);

        // Not invertible
        if(!RPT_Util::CanReconnect(alpha_lobe_k_min_1, 1, y_k_min_1, rc.x_k,
            rc.lobe_k_min_1, rc.lobe_k, alpha_min))
            return ret;

        float3 w_k_min_1 = normalize(rc.x_k - y_k_min_1);
        int16 bounce = rc.k - (int16)2;

        // Evaluate f / p at x_{k - 1}
        BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(normal_k_min_1, surface, w_k_min_1, 
            rc.lobe_k_min_1, rngReplay);

        if(dot(eval.bsdfOverPdf, eval.bsdfOverPdf) == 0)
            return ret;

        RtRayQuery::Hit hitInfo = RtRayQuery::Hit::FindClosest<true>(y_k_min_1, normal_k_min_1, 
            w_k_min_1, globals.bvh, globals.frameMeshData, globals.vertices, globals.indices, 
            surface.Transmissive());

        // Not invertible
        if(!hitInfo.hit || (hitInfo.ID != rc.ID))
            return ret;

        // Move to y_k = x_k
        const float3 y_k = mad(hitInfo.t, w_k_min_1, y_k_min_1);
        bounce++;

        // Update medium (from y_{k - 1} to x_k)
        const bool transmitted = dot(normal_k_min_1, w_k_min_1) < 0;
        eta_curr = transmitted ? (eta_curr == ETA_AIR ? eta_next : ETA_AIR) : eta_curr;
        const bool inTranslucentMedium = eta_curr != ETA_AIR;

        RtRayQuery::IsotropicSampler is;
        if(!RtRayQuery::GetMaterialData(-w_k_min_1, globals.materials, g_frame, eta_curr, 
            rd.uv_grads, hitInfo, surface, eta_next, g_samLinearWrap, is))
        {
            // Not invertible
            return ret;
        }

        // if(regularization && anyGlossyBounces)
        //     surface.Regularize();

        // Account for transmittance from y_{k - 1} to x_k
        if(inTranslucentMedium && (surface.trDepth > 0))
        {
            float3 extCoeff = -log(surface.baseColor_Fr0_TrCol) / surface.trDepth;
            beta *= exp(-hitInfo.t * extCoeff);
        }

        beta *= eval.bsdfOverPdf;
        ret.partialJacobian = eval.pdf;
        ret.partialJacobian *= abs(dot(-w_k_min_1, hitInfo.normal));
        ret.partialJacobian /= (hitInfo.t * hitInfo.t);

        // Case 1
        if(rc.lt_k_plus_1 == Light::TYPE::NONE)
        {
            float3 w_k = rc.w_k_lightNormal_w_sky;
            eval = BSDF::EvalBSDFSampler(hitInfo.normal, surface, w_k, 
                rc.lobe_k, rngReplay);

            beta *= eval.bsdfOverPdf;
            ret.target = beta * rc.L;
            ret.partialJacobian *= eval.pdf;

            return ret;
        }

        // Case 2: compute direct lighting at y_k(= x_k)
        if(Emissive)
        {
            float3 w_k = rc.w_k_lightNormal_w_sky;

            // Note: For temporal, following assumes visibility didn't change between frames
            ReSTIR_Util::DirectLightingEstimate ls = RPT_Util::EvalDirect_Emissive_Case2(y_k, 
                hitInfo.normal, surface, w_k, rc.L, rc.dwdA, rc.lightPdf, rc.lobe_k, 
                bounce, rngReplay, rngNEE);

            ret.target = beta * ls.ld;
            ret.partialJacobian *= ls.pdf_solidAngle;
        }
        else
        {
            // Was used for deciding between sun and sky
            rngNEE.Uniform();

            // Weighted reservoir sampling to sample Le x BSDF, with BSDF lobes as source distributions
            ReSTIR_Util::SkyIncidentRadiance leFunc = ReSTIR_Util::SkyIncidentRadiance::Init(
                g_frame.EnvMapDescHeapOffset);

            // TODO assumes visibility doesn't change. Not necessarily true for temporal.
            ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sun<false>(y_k, hitInfo.normal, 
                surface, globals.bvh, g_frame, rngNEE);

            const float pdf_sky = BSDF::BSDFSamplerPdf(hitInfo.normal, surface, -g_frame.SunDir, 
                rngNEE, leFunc);
            float3 target_z = ls.ld * ls.pdf_solidAngle;
            float w_sum = RT::BalanceHeuristic(1, pdf_sky, Math::Luminance(target_z));

            if(rc.lt_k_plus_1 == Light::TYPE::SKY)
            {
                BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(hitInfo.normal, surface, 
                    rc.w_k_lightNormal_w_sky, rc.lobe_k, leFunc, rngNEE);

                target_z = eval.f;
                w_sum += Math::Luminance(eval.bsdfOverPdf);
            }
            else
            {
                BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(hitInfo.normal, surface, 
                    leFunc, rngNEE);

                w_sum += Math::Luminance(bsdfSample.bsdfOverPdf);
            }

            const float targetLum = Math::Luminance(target_z);
            ret.target = beta * (targetLum > 0 ? target_z * w_sum / targetLum : 0);
            ret.partialJacobian *= w_sum > 0 ? targetLum / w_sum : 0;
        }

        return ret;
    }

    template<bool Emissive>
    OffsetPath Reconnect_Case3(float3 beta, float3 y_k_min_1, float3 normal_k_min_1, 
        BSDF::ShadingData surface, float alpha_min, Reconnection rc, 
        ConstantBuffer<cbFrameConstants> g_frame, ReSTIR_Util::Globals globals, 
        inout RNG rngReplay, inout RNG rngNEE)
    {
        OffsetPath ret = OffsetPath::Init();

        // Not invertible
        if(!BSDF::IsLobeValid(surface, rc.lobe_k_min_1))
            return ret;

        float alpha_lobe_k_min_1 = BSDF::LobeAlpha(surface, rc.lobe_k_min_1);

        // Not invertible
        if(alpha_lobe_k_min_1 < alpha_min)
            return ret;

        if(Emissive)
        {
            float3 wi_k_min_1 = rc.x_k - y_k_min_1;
            float t = length(wi_k_min_1);
            wi_k_min_1 /= t;

            float3 lightNormal = rc.w_k_lightNormal_w_sky;
            int16 bounce = rc.k - (int16)2;
            bool twoSided = rc.lightPdf > 0;

            ReSTIR_Util::DirectLightingEstimate ls = RPT_Util::EvalDirect_Emissive_Case3(y_k_min_1, 
                normal_k_min_1, surface, wi_k_min_1, t, rc.L, lightNormal, abs(rc.lightPdf), rc.ID, 
                twoSided, rc.lobe_k_min_1, bounce, globals.bvh, rngReplay, rngNEE);

            ret.target = beta * ls.ld;
            ret.partialJacobian = ls.pdf_solidAngle;
        }
        else
        {
            // Was used for deciding between sun and sky
            rngNEE.Uniform();

            // Weighted reservoir sampling to sample Le x BSDF, with BSDF lobes as source distributions
            ReSTIR_Util::SkyIncidentRadiance leFunc = ReSTIR_Util::SkyIncidentRadiance::Init(
                g_frame.EnvMapDescHeapOffset);

            float3 target_z = 0;
            float3 wi_sun = -g_frame.SunDir;
            float pdf_sky = 0;
            // Skip when Sun is below the horizon or it hits backside of non-transmissive surface
            const bool visible = (wi_sun.y > 0) &&
                ((dot(wi_sun, normal_k_min_1) > 0) || surface.Transmissive());

            if(visible)
            {
                surface.SetWi(wi_sun, normal_k_min_1);
                float3 f = BSDF::Unified(surface).f;
                target_z = Light::Le_Sun(y_k_min_1, g_frame) * f;

                pdf_sky = BSDF::BSDFSamplerPdf(normal_k_min_1, surface, wi_sun, 
                    rngNEE, leFunc);
            }

            float w_sum = RT::BalanceHeuristic(1, pdf_sky, Math::Luminance(target_z));

            if(rc.lt_k == Light::TYPE::SKY)
            {
                BSDF::BSDFSamplerEval eval = BSDF::EvalBSDFSampler(normal_k_min_1, surface, 
                    rc.w_k_lightNormal_w_sky, rc.lobe_k_min_1, leFunc, rngNEE);

                target_z = eval.f;
                w_sum += Math::Luminance(eval.bsdfOverPdf);
            }
            else
            {
                BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(normal_k_min_1, surface, 
                    leFunc, rngNEE);

                w_sum += Math::Luminance(bsdfSample.bsdfOverPdf);
            }

            const float targetLum = Math::Luminance(target_z);
            ret.target = beta * (targetLum > 0 ? target_z * w_sum / targetLum : 0);
            ret.partialJacobian = w_sum > 0 ? targetLum / w_sum : 0;

            if(dot(ret.target, ret.target) > 0)
            {
                float3 wi = rc.lt_k == Light::TYPE::SUN ? -g_frame.SunDir : 
                    rc.w_k_lightNormal_w_sky;
                ret.target *= RtRayQuery::Visibility_Ray(y_k_min_1, wi, normal_k_min_1, 
                    globals.bvh, surface.Transmissive());
            }
        }

        return ret;
    }

    template<bool Emissive>
    OffsetPath Shift2(uint2 DTid, float3 pos, float3 normal, float ior, 
        BSDF::ShadingData surface, RT::RayDifferentials rd, Math::TriDifferentials triDiffs, 
        RPT_Util::Reconnection rc, uint rbufferASrv, uint rbufferBSrv, uint rbufferCSrv, 
        uint rbufferDSrv, float alpha_min, bool regularization, ConstantBuffer<cbFrameConstants> g_frame, 
        ReSTIR_Util::Globals globals)
    {
        OffsetPathContext ctx = OffsetPathContext::Init();
        ctx.pos = pos;
        ctx.normal = normal;
        ctx.surface = surface;
        ctx.rngReplay = RNG::Init(rc.seed_replay);
        ctx.rd = rd;
        // Assumes camera is in the air
        ctx.eta_curr = ETA_AIR;
        ctx.eta_next = ior;
        // ctx.anyGlossyBounces = false;
        ctx.throughput = 1;

        // Number of bounces to reach y_{k - 1}, where zero means case 1 or case 2
        const int16 numBounces = rc.k - (int16)2;

        // Initialize ray differentials given the primary hit triangle. This is only
        // needed when replay is not used - replayed paths load path context (including ray 
        // differentials) from replay buffers.
        if(numBounces == 0)
        {
            float3 dpdx;
            float3 dpdy;
            ctx.rd.dpdx_dpdy(ctx.pos, ctx.normal, dpdx, dpdy);
            ctx.rd.ComputeUVDifferentials(dpdx, dpdy, triDiffs.dpdu, triDiffs.dpdv);

            float eta = ior;  // = eta_i / eta_t = ior / ETA_AIR
            float3 wi = normalize(rc.x_k - ctx.pos);
            ctx.rd.UpdateRays(ctx.pos, ctx.normal, wi, ctx.surface.wo, 
                triDiffs, dpdx, dpdy, dot(wi, ctx.normal) < 0, eta);
        }
        else
        {
            ctx = OffsetPathContext::Load(DTid, rbufferASrv, rbufferBSrv, rbufferCSrv, 
                rbufferDSrv, rc.IsCase3());

            if(dot(ctx.throughput, ctx.throughput) == 0)
                return RPT_Util::OffsetPath::Init();

            // Advance rng of path context to what it would be after replay
            for(int bounce = 0; bounce < numBounces; bounce++)
            {
                ctx.rngReplay.Uniform4D();
                ctx.rngReplay.Uniform4D();
                ctx.rngReplay.Uniform();
            }
        }

        RNG rngNEE = RNG::Init(rc.seed_nee);

        if(rc.IsCase3())
        {
            return RPT_Util::Reconnect_Case3<Emissive>(ctx.throughput, ctx.pos, ctx.normal, 
                ctx.surface, alpha_min, rc, g_frame, globals, ctx.rngReplay, rngNEE);
        }
        else
        {
            return RPT_Util::Reconnect_Case1Or2<Emissive>(ctx.throughput, ctx.pos, 
                ctx.normal, ctx.eta_curr, ctx.eta_next, ctx.rd, ctx.surface, false, 
                alpha_min, regularization, rc, g_frame, globals, ctx.rngReplay, rngNEE);
        }
    }

    OffsetPathContext Replay_kGt2(float3 pos, float3 normal, float ior, BSDF::ShadingData surface, 
        RT::RayDifferentials rd, Math::TriDifferentials triDiffs, Reconnection rc, 
        float alpha_min, bool regularization, SamplerState samp, 
        ConstantBuffer<cbFrameConstants> g_frame, ReSTIR_Util::Globals globals)
    {
        OffsetPathContext ctx = OffsetPathContext::Init();
        ctx.pos = pos;
        ctx.normal = normal;
        ctx.surface = surface;
        ctx.rngReplay = RNG::Init(rc.seed_replay);
        ctx.rd = rd;
        // Assumes camera is in the air
        ctx.eta_curr = ETA_AIR;
        ctx.eta_next = ior;
        // ctx.anyGlossyBounces = false;
        ctx.throughput = 1;

        // Number of bounces to reach y_{k - 1}, where zero means case 1 or case 2
        const int16 numBounces = rc.k - (int16)2;
        BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(ctx.normal, ctx.surface, ctx.rngReplay);

        if(dot(bsdfSample.bsdfOverPdf, bsdfSample.bsdfOverPdf) == 0)
        {
            ctx.throughput = 0;
            return ctx;
        }

        // Initialize ray differentials given the primary hit triangle
        float3 dpdx;
        float3 dpdy;
        ctx.rd.dpdx_dpdy(ctx.pos, ctx.normal, dpdx, dpdy);
        ctx.rd.ComputeUVDifferentials(dpdx, dpdy, triDiffs.dpdu, triDiffs.dpdv);

        ctx.rd.UpdateRays(ctx.pos, ctx.normal, bsdfSample.wi, ctx.surface.wo, 
            triDiffs, dpdx, dpdy, dot(bsdfSample.wi, ctx.normal) < 0, ctx.surface.eta);

        RPT_Util::Replay(numBounces, bsdfSample, alpha_min, regularization, g_frame, 
            samp, globals, ctx);

        return ctx;
    }
}

#endif