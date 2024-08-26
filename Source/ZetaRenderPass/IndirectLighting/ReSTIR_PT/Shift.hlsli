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
            ret.eta_mat = DEFAULT_ETA_I;
            ret.rngReplay.State = 0;
            ret.anyGlossyBounces = false;

            return ret;
        }
        
        static OffsetPathContext Load(uint2 DTid, uint srvAIdx, uint srvBIdx, uint srvCIdx, 
            bool isCase3)
        {
            Texture2D<float4> g_cbA = ResourceDescriptorHeap[srvAIdx];
            Texture2D<uint4> g_cbB = ResourceDescriptorHeap[srvBIdx];
            Texture2D<uint4> g_cbC = ResourceDescriptorHeap[srvCIdx];

            float3 inA = g_cbA[DTid].xyz;
            uint4 inB = g_cbB[DTid];
            uint4 inC = g_cbC[DTid];

            OffsetPathContext ctx = OffsetPathContext::Init();
            ctx.throughput = inA.xyz;

            if(dot(ctx.throughput, ctx.throughput) == 0)
                return ctx;

            ctx.pos = asfloat(inB.xyz);
            ctx.normal = Math::DecodeOct32(Math::UnpackUintToUint16(inB.w));
            ctx.eta_curr = mad(Math::UNorm8ToFloat((inC.z >> 8) & 0xff), 1.5f, 1.0f);
            ctx.eta_mat = mad(Math::UNorm8ToFloat((inC.z >> 16) & 0xff), 1.5f, 1.0f);

            float3 wo = Math::DecodeOct32(Math::UnpackUintToUint16(inC.x));
            float roughness = Math::UNorm8ToFloat(inC.z & 0xff);
            float3 baseColor = Math::UnpackRGB8(inC.y & 0xffffff);
            uint flags = inC.y >> 24;
            bool metallic = flags & 0x1;
            ctx.anyGlossyBounces = (flags & 0x2) == 0x2;
            bool specTr = (flags & 0x4) == 0x4;
            half trDepth = (flags & 0x8) == 0x8 ? 1.0h : 0.0h;
            float subsurface = Math::UNorm8ToFloat((inC.z >> 24) & 0xff);

            // UNorm8 encoding above preserves ETA_AIR = 1, so the comparison
            // below works
            float eta_t = ctx.eta_curr == ETA_AIR ? ETA_AIR : ctx.eta_mat;
            float eta_i = ctx.eta_curr == ETA_AIR ? ctx.eta_mat : ETA_AIR;

            ctx.surface = BSDF::ShadingData::Init(ctx.normal, wo, metallic, roughness, 
                baseColor, eta_i, eta_t, specTr, trDepth, (half)subsurface);

            if(!isCase3)
                ctx.rd.uv_grads.xy = asfloat16(uint16_t2(inC.w & 0xffff, inC.w >> 16));

            return ctx;
        }

        void Write(uint2 DTid, uint uavAIdx, uint uavBIdx, uint uavCIdx, bool isCase3)
        {
            uint16_t2 e1 = Math::EncodeOct32(this.normal);
            uint normal_e = e1.x | (uint(e1.y) << 16);

            // ShadingData
            uint16_t2 e2 = Math::EncodeOct32(this.surface.wo);
            uint wo = e2.x | (uint(e2.y) << 16);
            // Note: needed so that BSDF evaulation at x_{k - 1} uses the right
            // transmission tint
            bool hasVolumetricInterior = surface.trDepth > 0;
            uint flags = (uint)surface.metallic | 
                ((uint)this.anyGlossyBounces << 1) | 
                ((uint)surface.specTr << 2) |
                ((uint)hasVolumetricInterior << 3);
            uint baseColor_Flags = Math::Float3ToRGB8(surface.diffuseReflectance_Fr0_TrCol);
            baseColor_Flags = baseColor_Flags | (flags << 24);
            uint roughness = Math::FloatToUNorm8(!surface.specular ? sqrt(surface.alpha) : 0);
            uint eta_curr = Math::FloatToUNorm8((this.eta_curr - 1.0f) / 1.5f);
            uint eta_mat = Math::FloatToUNorm8((this.eta_mat - 1.0f) / 1.5f);
            uint subsurface = Math::FloatToUNorm8((float)surface.subsurface);
            uint packed = roughness | (eta_curr << 8) | (eta_mat << 16) | (subsurface << 24);

            // R16G16B16A16_FLOAT
            RWTexture2D<float4> g_rbA = ResourceDescriptorHeap[uavAIdx];
            // R32G32B32A32_UINT
            RWTexture2D<uint4> g_rbB = ResourceDescriptorHeap[uavBIdx];
            // R32G32B32A32_UINT
            RWTexture2D<uint4> g_rbC = ResourceDescriptorHeap[uavCIdx];

            g_rbA[DTid].xyz = this.throughput;
            g_rbB[DTid] = uint4(asuint(this.pos), normal_e);

            if(!isCase3)
            {
                float ddx_uv = sqrt(dot(this.rd.uv_grads.xy, this.rd.uv_grads.xy));
                float ddy_uv = sqrt(dot(this.rd.uv_grads.zw, this.rd.uv_grads.zw));

                uint16_t2 grads_h = asuint16(half2(ddx_uv, ddy_uv));
                g_rbC[DTid] = uint4(wo, baseColor_Flags, packed, grads_h.x | (uint(grads_h.y) << 16));
            }
            // texture filtering is not needed
            else
                g_rbC[DTid].xyz = uint3(wo, baseColor_Flags, packed);
        }

        float3 throughput;
        float3 pos;
        float3 normal;
        RT::RayDifferentials rd;
        BSDF::ShadingData surface;
        float eta_curr;
        float eta_mat;
        RNG rngReplay;
        bool anyGlossyBounces;
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
        ctx.eta_curr = dot(ctx.normal, bsdfSample.wi) < 0 ? ctx.eta_mat : ETA_AIR;
        bool inTranslucentMedium = ctx.eta_curr != ETA_AIR;
        // Note: skip the first bounce for a milder impacet. May have to change in the future.
        // ctx.anyGlossyBounces = bsdfSample.lobe != BSDF::LOBE::DIFFUSE_R;
        ctx.anyGlossyBounces = false;

        float alpha_lobe_prev = BSDF::LobeAlpha(ctx.surface.alpha, bsdfSample.lobe);
        bool tr_prev = ctx.surface.specTr;
        BSDF::LOBE lobe_prev = bsdfSample.lobe;
        float3 pos_prev = ctx.pos;

        while(true)
        {
            ReSTIR_RT::Hit hitInfo = ReSTIR_RT::Hit::FindClosest<false>(ctx.pos, ctx.normal, 
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

            if(!ReSTIR_RT::GetMaterialData(-bsdfSample.wi, globals.materials, g_frame, ctx.eta_curr, 
                ctx.rd.uv_grads, hitInfo, ctx.surface, ctx.eta_mat, samp))
            {
                // Not invertible
                ctx.throughput = 0;
                return;
            }

            // Make sure these are updated before breaking
            ctx.pos = mad(hitInfo.t, bsdfSample.wi, ctx.pos);
            ctx.normal = hitInfo.normal;
            bounce++;

            if(regularization && ctx.anyGlossyBounces)
                ctx.surface.Regularize();

            if(inTranslucentMedium && (ctx.surface.trDepth > 0))
            {
                float3 extCoeff = -log(ctx.surface.diffuseReflectance_Fr0_TrCol) / ctx.surface.trDepth;
                ctx.throughput *= exp(-hitInfo.t * extCoeff);
            }

            // Remaining code can be skipped in last iteration
            if(bounce >= numBounces)
                break;

            // Sample BSDF to generate new direction
            bsdfSample = BSDF::BSDFSample::Init();
            bool sampleNonDiffuse = (bounce < globals.maxGlossyBounces_NonTr) ||
                (ctx.surface.specTr && (bounce < globals.maxGlossyBounces_Tr));

            if(bounce < globals.maxDiffuseBounces)
                bsdfSample = BSDF::SampleBSDF(ctx.normal, ctx.surface, ctx.rngReplay);
            else if(sampleNonDiffuse)
                bsdfSample = BSDF::SampleBSDF_NoDiffuse(ctx.normal, ctx.surface, ctx.rngReplay);

            // Not invertible -- base path would've stopped here
            if(dot(bsdfSample.bsdfOverPdf, bsdfSample.bsdfOverPdf) == 0)
            {
                ctx.throughput = 0;
                return;
            }

            const float alpha_lobe = BSDF::LobeAlpha(ctx.surface.alpha, bsdfSample.lobe);

            // Not invertible -- reconnection vertex must match
            if(RPT_Util::CanReconnect(alpha_lobe_prev, alpha_lobe, pos_prev, ctx.pos, lobe_prev, 
                bsdfSample.lobe, alpha_min))
            {
                ctx.throughput = 0;
                return;
            }

            bool transmitted = dot(ctx.normal, bsdfSample.wi) < 0;
            ctx.eta_curr = transmitted ? (ctx.eta_curr == ETA_AIR ? ctx.eta_mat : ETA_AIR) : ctx.eta_curr;
            ctx.throughput *= bsdfSample.bsdfOverPdf;
            ctx.anyGlossyBounces = ctx.anyGlossyBounces || (bsdfSample.lobe != BSDF::LOBE::DIFFUSE_R);
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
    //  - eta_mat: IOR of material at vertex y_{k - 1}
    template<bool Emissive>
    OffsetPath Reconnect_Case1Or2(float3 beta, float3 y_k_min_1, float3 normal_k_min_1, 
        float eta_curr, float eta_mat, RT::RayDifferentials rd, BSDF::ShadingData surface, 
        bool anyGlossyBounces, float alpha_min, bool regularization, Reconnection rc, 
        ConstantBuffer<cbFrameConstants> g_frame, ReSTIR_Util::Globals globals, 
        RNG rngReplay, RNG rngNEE)
    {
        OffsetPath ret = OffsetPath::Init();

        // Not invertible
        if(!BSDF::IsLobeValid(surface, rc.lobe_k_min_1))
            return ret;

        float alpha_lobe_k_min_1 = BSDF::LobeAlpha(surface.alpha, rc.lobe_k_min_1);

        // Not invertible
        if(!RPT_Util::CanReconnect(alpha_lobe_k_min_1, 1, y_k_min_1, rc.x_k,
            rc.lobe_k_min_1, rc.lobe_k, alpha_min))
            return ret;

        float3 w_k_min_1 = normalize(rc.x_k - y_k_min_1);
        int16 bounce = rc.k - (int16)2;

        // Evaluate f / p at x_{k - 1}
        BSDF::BSDFSamplerEval eval;

        if(bounce < globals.maxDiffuseBounces)
        {
            eval = BSDF::EvalBSDFSampler(normal_k_min_1, surface, w_k_min_1, 
                rc.lobe_k_min_1, rngReplay);
        }
        else
        {
            eval = BSDF::EvalBSDFSampler_NoDiffuse(normal_k_min_1, surface, w_k_min_1, 
                rc.lobe_k_min_1, rngReplay);
        }

        if(dot(eval.bsdfOverPdf, eval.bsdfOverPdf) == 0)
            return ret;

        ReSTIR_RT::Hit hitInfo = ReSTIR_RT::Hit::FindClosest<true>(y_k_min_1, normal_k_min_1, 
            w_k_min_1, globals.bvh, globals.frameMeshData, globals.vertices, globals.indices, 
            surface.Transmissive());

        // Not invertible
        if(!hitInfo.hit || (hitInfo.ID != rc.ID))
            return ret;

        // Move to y_k = x_k
        const float3 y_k = mad(hitInfo.t, w_k_min_1, y_k_min_1);
        bounce++;
        bool transmitted = dot(normal_k_min_1, w_k_min_1) < 0;
        eta_curr = transmitted ? (eta_curr == ETA_AIR ? eta_mat : ETA_AIR) : eta_curr;
        bool inTranslucentMedium = eta_curr != ETA_AIR;

        ReSTIR_RT::IsotropicSampler is;
        if(!ReSTIR_RT::GetMaterialData(-w_k_min_1, globals.materials, g_frame, eta_curr, 
            rd.uv_grads, hitInfo, surface, eta_mat, g_samLinearWrap, is))
        {
            // Not invertible
            return ret;
        }

        if(regularization && anyGlossyBounces)
            surface.Regularize();

        if(inTranslucentMedium && (surface.trDepth > 0))
        {
            float3 extCoeff = -log(surface.diffuseReflectance_Fr0_TrCol) / surface.trDepth;
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
            if(bounce < globals.maxDiffuseBounces)
            {
                eval = BSDF::EvalBSDFSampler(hitInfo.normal, surface, w_k, 
                    rc.lobe_k, rngReplay);
            }
            else
            {
                eval = BSDF::EvalBSDFSampler_NoDiffuse(hitInfo.normal, surface, w_k, 
                    rc.lobe_k, rngReplay);
            }

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
                bounce, globals.maxDiffuseBounces, rngReplay, rngNEE);

            ret.target = beta * ls.ld;
            ret.partialJacobian *= ls.pdf_solidAngle;
        }
        else
        {
            // Was used for deciding between sun and sky
            rngNEE.Uniform();

            if(rc.lt_k_plus_1 == Light::TYPE::SUN)
            {
                // Note: assumes visibility doesn't change. Not necessarily true for temporal.
                ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sun<false>(y_k, hitInfo.normal, 
                    surface, globals.bvh, g_frame, rngNEE);

                ret.target = beta * ls.ld;
            }
            else if(rc.lt_k_plus_1 == Light::TYPE::SKY)
            {
                float3 wi = rc.w_k_lightNormal_w_sky;

                // Note: assumes visibility doesn't change. Not necessarily true for temporal.
                ReSTIR_Util::DirectLightingEstimate ls = RPT_Util::EvalDirect_Sky<false>(y_k, 
                    hitInfo.normal, surface, wi, rc.lobe_k, g_frame.EnvMapDescHeapOffset, 
                    globals.bvh, rngNEE);

                ret.target = beta * ls.ld;
                ret.partialJacobian *= ls.pdf_solidAngle;
            }
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

        float alpha_lobe_k_min_1 = BSDF::LobeAlpha(surface.alpha, rc.lobe_k_min_1);

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
                twoSided, rc.lobe_k_min_1, bounce, globals.maxDiffuseBounces, globals.bvh, rngReplay, 
                rngNEE);

            ret.target = beta * ls.ld;
            ret.partialJacobian = ls.pdf_solidAngle;
        }
        else
        {
            // Was used for deciding between sun and sky
            rngNEE.Uniform();

            if(rc.lt_k == Light::TYPE::SUN)
            {
                ReSTIR_Util::DirectLightingEstimate ls = ReSTIR_Util::NEE_Sun<true>(y_k_min_1, 
                    normal_k_min_1, surface, globals.bvh, g_frame, rngNEE);
                ret.target = beta * ls.ld;
                ret.partialJacobian = 1;
            }
            else if(rc.lt_k == Light::TYPE::SKY)
            {
                float3 wi = rc.w_k_lightNormal_w_sky;

                ReSTIR_Util::DirectLightingEstimate ls = RPT_Util::EvalDirect_Sky<true>(y_k_min_1, 
                    normal_k_min_1, surface, wi, rc.lobe_k_min_1, g_frame.EnvMapDescHeapOffset, 
                    globals.bvh, rngNEE);

                ret.target = beta * ls.ld;
                ret.partialJacobian = ls.pdf_solidAngle;
            }
        }

        return ret;
    }

    template<bool Emissive>
    OffsetPath Shift2(uint2 DTid, float3 pos, float3 normal, float ior, BSDF::ShadingData surface, 
        RT::RayDifferentials rd, Math::TriDifferentials triDiffs, RPT_Util::Reconnection rc, 
        uint rbufferASrv, uint rbufferBSrv, uint rbufferCSrv, float alpha_min, 
        bool regularization, ConstantBuffer<cbFrameConstants> g_frame, ReSTIR_Util::Globals globals)
    {
        OffsetPathContext ctx = OffsetPathContext::Init();
        ctx.pos = pos;
        ctx.normal = normal;
        ctx.surface = surface;
        ctx.rngReplay = RNG::Init(rc.seed_replay);
        ctx.rd = rd;
        // Assumes camera is in the air
        ctx.eta_curr = ETA_AIR;
        ctx.eta_mat = ior;
        ctx.anyGlossyBounces = false;
        ctx.throughput = 1;

        // Number of bounces to reach y_{k - 1}, where zero means case 1 or case 2
        const int16 numBounces = rc.k - (int16)2;
        BSDF::BSDFSample bsdfSample;

        if(numBounces > 0)
        {
            bsdfSample = BSDF::SampleBSDF(ctx.normal, ctx.surface, ctx.rngReplay);

            if(dot(bsdfSample.bsdfOverPdf, bsdfSample.bsdfOverPdf) == 0)
                return RPT_Util::OffsetPath::Init();
        }
        else
            bsdfSample.wi = rc.x_k - ctx.pos;

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
            ctx.rd.UpdateRays(ctx.pos, ctx.normal, bsdfSample.wi, ctx.surface.wo, 
                triDiffs, dpdx, dpdy, dot(bsdfSample.wi, ctx.normal) < 0, eta);
        }

        if(numBounces > 0)
        {
            int16 bounce = 0;
            ctx = OffsetPathContext::Load(DTid, rbufferASrv, rbufferBSrv, rbufferCSrv, rc.IsCase3());

            // Advance rng of path context to what it would be after replay
            while(true)
            {
                bounce++;

                if(bounce >= numBounces)
                    break;

                if(bounce < globals.maxDiffuseBounces)
                {
                    ctx.rngReplay.Uniform3D();
                    ctx.rngReplay.Uniform3D();
                }
                else
                    ctx.rngReplay.Uniform3D();
            }
        }

        if(dot(ctx.throughput, ctx.throughput) == 0)
            return RPT_Util::OffsetPath::Init();

        RNG rngNEE = RNG::Init(rc.seed_nee);

        if(rc.IsCase3())
        {
            return RPT_Util::Reconnect_Case3<Emissive>(ctx.throughput, ctx.pos, ctx.normal, 
                ctx.surface, alpha_min, rc, g_frame, globals, ctx.rngReplay, rngNEE);
        }
        else
        {
            return RPT_Util::Reconnect_Case1Or2<Emissive>(ctx.throughput, ctx.pos, 
                ctx.normal, ctx.eta_curr, ctx.eta_mat, ctx.rd, ctx.surface, ctx.anyGlossyBounces, 
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
        ctx.eta_mat = ior;
        ctx.anyGlossyBounces = false;
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