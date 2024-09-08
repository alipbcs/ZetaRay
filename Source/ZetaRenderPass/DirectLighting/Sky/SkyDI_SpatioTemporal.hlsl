#include "SkyDI_Reservoir.hlsli"
#include "../../Common/Common.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/RT.hlsli"
#include "../../Common/Volumetric.hlsli"
#include "../../Common/BSDFSampling.hlsli"

#define THREAD_GROUP_SWIZZLING 1
#define MIN_ROUGHNESS_SURFACE_MOTION 0.4
#define MAX_NUM_TEMPORAL_SAMPLES 4
#define MAX_PLANE_DIST_REUSE 0.0025
#define MIN_NORMAL_SIMILARITY_REUSE 0.906307787    // within 25 degrees
#define MAX_ROUGHNESS_DIFF_REUSE 0.1f
#define NUM_SPATIAL_SAMPLES 2
#define SPATIAL_SEARCH_RADIUS 16

struct TemporalCandidate
{
    static TemporalCandidate Init()
    {
        TemporalCandidate ret;
        ret.valid = false;

        return ret;
    }

    float3 pos;
    float roughness;
    float3 normal;
    int16_t2 posSS;
    float eta_next;
    bool transmissive;
    bool valid;
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI_Temporal> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool Visibility(float3 pos, float3 wi, float3 normal, bool transmissive)
{
    if(wi.y < 0)
        return false;

    float ndotwi = dot(normal, wi);
    if(ndotwi == 0)
        return false;

    bool wiBackface = ndotwi < 0;

    if(wiBackface)
    {
        if(transmissive)
            normal *= -1;
        else
            return false;
    }

    const float3 adjustedOrigin = RT::OffsetRayRTG(pos, normal);

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
        RAY_FLAG_FORCE_OPAQUE> rayQuery;

    RayDesc ray;
    ray.Origin = adjustedOrigin;
    ray.TMin = wiBackface ? 1e-6 : 0;
    ray.TMax = FLT_MAX;
    ray.Direction = wi;

    rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);
    rayQuery.Proceed();

    if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        return false;

    return true;
}

float3 Le(float3 pos, float3 normal, float3 wi, BSDF::ShadingData surface)
{
    const bool vis = Visibility(pos, wi, normal, surface.Transmissive());
    if (!vis)
        return 0.0;

    // Sample sky-view LUT
    Texture2D<float4> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
    const float2 thetaPhi = Math::SphericalFromCartesian(wi);
    float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

    // Undo non-linear sampling
    const float s = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
    uv.y = (thetaPhi.x - PI_OVER_2) * 0.5f;
    uv.y = 0.5f + s * sqrt(abs(uv.y) * ONE_OVER_PI);
    
    return g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f).rgb;
}

// Ref: Bitterli, Benedikt, "Correlations and Reuse for Fast and Accurate Physically Based Light Transport" (2022). Ph.D Dissertation.
// https://digitalcommons.dartmouth.edu/dissertations/77
struct PairwiseMIS
{    
    static PairwiseMIS Init(uint16_t numStrategies, SkyDI_Util::Reservoir r_c)
    {
        PairwiseMIS ret;

        ret.r_s = SkyDI_Util::Reservoir::Init();
        ret.m_c = 1.0f;
        ret.M_s = r_c.M;
        ret.k = numStrategies;

        return ret;
    }

    float Compute_m_i(SkyDI_Util::Reservoir r_c, float targetLum, SkyDI_Util::Reservoir r_i, 
        float w_sum_i)
    {
        const float p_i_y_i = r_i.W > 0 ? w_sum_i / r_i.W : 0;
        const float p_c_y_i = targetLum;
        float numerator = (float)r_i.M * p_i_y_i;
        float denom = numerator + ((float)r_c.M / this.k) * p_c_y_i;
        float m_i = denom > 0 ? numerator / denom : 0;

        return m_i;
    }

    void Update_m_c(SkyDI_Util::Reservoir r_c, SkyDI_Util::Reservoir r_i, float3 brdfCosTheta_i)
    {
        const float target_i = Math::Luminance(r_c.Le * brdfCosTheta_i);
        const float p_i_y_c = target_i;
        const float p_c_y_c = Math::Luminance(r_c.Target);

        const float numerator = (float)r_i.M * p_i_y_c;
        const float denom = numerator + ((float)r_c.M / this.k) * p_c_y_c;
        // Note: denom can never be zero, otherwise r_c didn't have a valid sample
        // and this function shouldn't have been called
        this.m_c += 1 - (numerator / denom);
    }

    void Stream(SkyDI_Util::Reservoir r_c, float3 pos_c, float3 normal_c, 
        BSDF::ShadingData surface_c, SkyDI_Util::Reservoir r_i, float3 pos_i, 
        float3 normal_i, float w_sum_i, BSDF::ShadingData surface_i, inout RNG rng)
    {
        float3 currTarget = 0;
        float m_i = 0;

        // m_i
        if(r_i.IsValid())
        {
            surface_c.SetWi(r_i.wi, normal_c);
            const float3 brdfCosTheta_c = BSDF::UnifiedBSDF(surface_c).f;
            currTarget = r_i.Le * brdfCosTheta_c;

            if(dot(currTarget, currTarget) > 0)
            {
                currTarget *= Visibility(pos_c, r_i.wi, normal_c, 
                    surface_c.Transmissive());
            }

            const float targetLum = Math::Luminance(currTarget);
            m_i = Compute_m_i(r_c, targetLum, r_i, w_sum_i);
        }

        float3 brdfCosTheta_i = 0;

        // m_c
        if(r_c.IsValid())
        {
            surface_i.SetWi(r_c.wi, normal_i);
            brdfCosTheta_i = BSDF::UnifiedBSDF(surface_i).f;

            if(dot(brdfCosTheta_i, brdfCosTheta_i) > 0)
            {
                brdfCosTheta_i *= Visibility(pos_i, r_c.wi, normal_i, 
                    surface_i.Transmissive());
            }

            Update_m_c(r_c, r_i, brdfCosTheta_i);
        }    

        if(r_i.IsValid())
        {
            const float w_i = m_i * Math::Luminance(currTarget) * r_i.W;
            this.r_s.Update(w_i, r_i.Le, r_i.wi, currTarget, rng);
        }

        this.M_s += r_i.M;
    }

    void End(SkyDI_Util::Reservoir r_c, inout RNG rng)
    {
        // r_c.w_sum = Luminance(r_c.Target) * r_c.W
        const float w_c = r_c.w_sum * this.m_c;
        this.r_s.Update(w_c, r_c.Le, r_c.wi, r_c.Target, rng);
        this.r_s.M = this.M_s;

        const float targetLum = Math::Luminance(r_s.Target);
        this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
    }

    SkyDI_Util::Reservoir r_s;
    float m_c;
    half M_s;
    uint16_t k;
};

SkyDI_Util::Reservoir RIS_InitialCandidates(uint2 DTid, float3 pos, float3 normal, bool metallic,
    float roughness, BSDF::ShadingData surface, inout RNG rng)
{
    SkyDI_Util::Reservoir r = SkyDI_Util::Reservoir::Init();

    // MIS -- take a light sample and a BSDF sample

    // Sky is low frequency, so instead use cosine-weighted hemisphere sampling as 
    // a proxy for light sampling
    {
        const float2 u = rng.Uniform2D();
        float p_e;
        float3 wi_e = BSDF::SampleDiffuseRefl(normal, u, p_e);
        const float3 le = Le(pos, normal, wi_e, surface);

        surface.SetWi(wi_e, normal);
        const float3 target = le * BSDF::UnifiedBSDF(surface).f;

        // Balance heuristic
        // p_e in m_e's numerator and w_e's denominator cancel out
        const float numerator = 1.0f;
        const float denom = p_e + BSDF::BSDFSamplerPdf_NoDiffuse(normal, surface, wi_e);
        const float m_e = denom > 0 ? numerator / denom : 0;
        const float w_e = m_e * Math::Luminance(target);

        r.Update(w_e, le, wi_e, target, rng);
    }

    // Sample non-diffuse BSDF
    {
        BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF_NoDiffuse(normal, surface, rng);
        float3 wi_s = bsdfSample.wi;
        float3 f = bsdfSample.f;
        float p_s = bsdfSample.pdf;

        float ndotwi = saturate(dot(wi_s, normal));
        const float p_e = ndotwi * ONE_OVER_PI;
        const float3 le = Le(pos, normal, wi_s, surface);
        const float3 target = le * f;

        // p_s in m_s's numerator and w_s's denominator cancel out
        const float numerator = 1.0f;
        const float denom = p_s + p_e;
        const float m_s = denom > 0 ? numerator / denom : 0;
        const float w_s = m_s * Math::Luminance(target);

        r.Update(w_s, le, wi_s, target, rng);
    }

    float targetLum = Math::Luminance(r.Target);
    r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;

    return r;
}

bool PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float linearDepth,
    float tolerance = MAX_PLANE_DIST_REUSE)
{
    float planeDist = dot(currNormal, samplePos - currPos);
    bool weight = abs(planeDist) <= tolerance * linearDepth;

    return weight;
}

TemporalCandidate FindTemporalCandidate(uint2 DTid, float3 pos, float3 normal, float z_view, 
    bool metallic, float roughness, BSDF::ShadingData surface, inout RNG rng)
{
    TemporalCandidate candidate = TemporalCandidate::Init();
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

    // reverse reproject current pixel
    GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::MOTION_VECTOR];
    const float2 motionVec = g_motionVector[DTid];
    const float2 prevUV_surface = (DTid + 0.5) / renderDim - motionVec;
    // TODO surface motion can be laggy for glossy surfaces
    float2 prevUV = prevUV_surface;

    if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
        return candidate;

    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    GBUFFER_IOR g_prevIOR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::IOR];

    const int2 prevPixel = prevUV * renderDim;
    const float3 prevCamPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
        g_frame.PrevViewInv._m23);

    for(int i = 0; i < MAX_NUM_TEMPORAL_SAMPLES; i++)
    {
        const float theta = rng.Uniform() * TWO_PI;
        const float sinTheta = sin(theta);
        const float cosTheta = cos(theta);
        const int2 samplePosSS = prevPixel + (i > 0) * 8 * float2(sinTheta, cosTheta);

        if(samplePosSS.x >= renderDim.x || samplePosSS.y >= renderDim.y)
            continue;

        const float2 prevMR = g_prevMR[samplePosSS];
        GBuffer::Flags prevFlags = GBuffer::DecodeMetallic(prevMR.x);

        if(prevFlags.invalid || prevFlags.emissive)
            continue;

        // plane-based heuristic
        float prevDepth = g_prevDepth[samplePosSS];
        float2 lensSample = 0;
        float3 origin = prevCamPos;
        if(g_frame.DoF)
        {
            RNG rngDoF = RNG::Init(RNG::PCG3d(samplePosSS.xyx).zy, g_frame.FrameNum - 1);
            lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
            lensSample *= g_frame.LensRadius;
        }

        float3 prevPos = Math::WorldPosFromScreenSpace2(samplePosSS, renderDim, prevDepth, 
            g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
            g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
            g_frame.DoF, lensSample, g_frame.FocusDepth, origin);

        float tolerance = MAX_PLANE_DIST_REUSE * (g_frame.DoF ? 10 : 1);
        if(!PlaneHeuristic(prevPos, normal, pos, z_view, tolerance))
            continue;

        // normal heuristic
        const float2 prevNormalEncoded = g_prevNormal[samplePosSS];
        const float3 prevNormal = Math::DecodeUnitVector(prevNormalEncoded);
        const float normalSimilarity = dot(prevNormal, normal);
            
        // roughness heuristic
        const bool roughnessSimilarity = abs(prevMR.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE;
        candidate.valid = g_frame.DoF ? true : 
            normalSimilarity >= MIN_NORMAL_SIMILARITY_REUSE && 
            roughnessSimilarity && (metallic == prevFlags.metallic);

        if(candidate.valid)
        {
            float prevEta_mat = DEFAULT_ETA_MAT;

            if(prevFlags.transmissive)
            {
                float ior = g_prevIOR[samplePosSS];
                prevEta_mat = GBuffer::DecodeIOR(ior);
            }

            candidate.posSS = (int16_t2)samplePosSS;
            candidate.pos = prevPos;
            candidate.normal = prevNormal;
            candidate.roughness = prevMR.y;
            candidate.transmissive = prevFlags.transmissive;
            candidate.eta_next = prevEta_mat;

            break;
        }
    }

    return candidate;
}

void TemporalResample(TemporalCandidate candidate, float3 pos, float3 normal, bool metallic,
    BSDF::ShadingData surface, inout SkyDI_Util::Reservoir r, inout RNG rng)
{
    SkyDI_Util::Reservoir prev = SkyDI_Util::PartialReadReservoir_Reuse(candidate.posSS, 
        g_local.PrevReservoir_A_DescHeapIdx);
    const half newM = r.M + prev.M;

    if(r.w_sum != 0)
    {
        GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];

        float targetLumAtPrev = 0.0f;

        if(dot(r.Le, r.Le) > 0)
        {
            const float3 prevBaseColor = g_prevBaseColor[candidate.posSS].rgb;
            float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
                g_frame.PrevViewInv._m23);
            if(g_frame.DoF)
            {
                RNG rngDoF = RNG::Init(RNG::PCG3d(candidate.posSS.xyx).zy, g_frame.FrameNum - 1);
                float2 lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
                lensSample *= g_frame.LensRadius;

                prevCameraPos += mad(lensSample.x, g_frame.PrevView[0].xyz, 
                    lensSample.y * g_frame.PrevView[1].xyz);
            }
            const float3 prevWo = normalize(prevCameraPos - candidate.pos);

            BSDF::ShadingData prevSurface = BSDF::ShadingData::Init(candidate.normal, prevWo,
                metallic, candidate.roughness, prevBaseColor, ETA_AIR, candidate.eta_next, 
                candidate.transmissive);

            prevSurface.SetWi(r.wi, candidate.normal);

            const float3 targetAtPrev = r.Le * BSDF::UnifiedBSDF(prevSurface).f;
            targetLumAtPrev = Math::Luminance(targetAtPrev);

            if(targetLumAtPrev > 0)
            {
                targetLumAtPrev *= Visibility(candidate.pos, r.wi, candidate.normal, 
                    prevSurface.Transmissive());
            }
        }

        const float p_curr = (float)r.M * Math::Luminance(r.Target);
        float numerator = p_curr;
        float denom = numerator + (float)prev.M * targetLumAtPrev;
        const float m_curr = denom > 0 ? numerator / denom : 0;
        r.w_sum *= m_curr;
    }

    if(prev.IsValid())
    {
        // compute target at current pixel with temporal reservoir's sample
        surface.SetWi(prev.wi, normal);
        const float3 currTarget = prev.Le * BSDF::UnifiedBSDF(surface).f;
        float targetLumAtCurr = Math::Luminance(currTarget);
        
        if(targetLumAtCurr > 0)
            targetLumAtCurr *= Visibility(pos, prev.wi, normal, surface.Transmissive());
        
        // w_prev becomes zero; then only M needs to be updated, which is done at the end anyway
        if(targetLumAtCurr > 0)
        {
            const float w_sum_prev = SkyDI_Util::PartialReadReservoir_WSum(candidate.posSS, g_local.PrevReservoir_B_DescHeapIdx);
            const float targetLumAtPrev = prev.W > 0 ? w_sum_prev / prev.W : 0;
            const float numerator = (float)prev.M * targetLumAtPrev;
            const float denom = numerator + (float)r.M * targetLumAtCurr;
            // balance heuristic
            const float m_prev = denom > 0 ? numerator / denom : 0;
            const float w_prev = m_prev * targetLumAtCurr * prev.W;

            r.Update(w_prev, prev.Le, prev.wi, currTarget, rng);
        }
    }

    float targetLum = Math::Luminance(r.Target);
    r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
    r.M = newM;
}

void SpatialResample(uint2 DTid, uint16_t numSamples, float radius, float3 pos, float3 normal, 
    float linearDepth, float roughness, BSDF::ShadingData surface, uint prevReservoir_A_DescHeapIdx, 
    uint prevReservoir_B_DescHeapIdx, inout SkyDI_Util::Reservoir r, inout RNG rng)
{
    static const half2 k_hammersley[8] =
    {
        half2(0.0, -0.7777777777777778),
        half2(-0.5, -0.5555555555555556),
        half2(0.5, -0.33333333333333337),
        half2(-0.75, -0.11111111111111116),
        half2(0.25, 0.11111111111111116),
        half2(-0.25, 0.33333333333333326),
        half2(0.75, 0.5555555555555556),
        half2(-0.875, 0.7777777777777777)
    };

    GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    GBUFFER_METALLIC_ROUGHNESS g_prevMR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    GBUFFER_IOR g_prevIOR = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
        GBUFFER_OFFSET::IOR];

    // rotate sample sequence per pixel
    const float u0 = rng.Uniform();
    const int offset = (int)rng.UniformUintBounded_Faster(8);
    const float theta = u0 * TWO_PI;
    const float sinTheta = sin(theta);
    const float cosTheta = cos(theta);

    const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
    PairwiseMIS pairwiseMIS = PairwiseMIS::Init(numSamples, r);

    float3 samplePos[3];
    float3 sampleOrigin[3];
    int16_t2 samplePosSS[3];
    float sampleRoughness[3];
    bool sampleMetallic[3];
    bool sampleTransmissive[3];
    float sampleEta_mat[3];
    uint16_t k = 0;
    const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, 
        g_frame.PrevViewInv._m23);

    for (int i = 0; i < numSamples; i++)
    {
        float2 sampleUV = k_hammersley[(offset + i) & 7];
        float2 rotated;
        rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
        rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
        rotated *= radius;
        const int2 posSS_i = round(float2(DTid) + rotated);

        if (Math::IsWithinBounds(posSS_i, (int2)renderDim))
        {
            const float2 mr_i = g_prevMR[posSS_i];
            GBuffer::Flags flags_i = GBuffer::DecodeMetallic(mr_i.x);

            if (flags_i.invalid || flags_i.emissive)
                continue;

            const float depth_i = g_prevDepth[posSS_i];
            float2 lensSample = 0;
            float3 origin_i = prevCameraPos;
            if(g_frame.DoF)
            {
                RNG rngDoF = RNG::Init(RNG::PCG3d(posSS_i.xyx).zy, g_frame.FrameNum - 1);
                lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
                lensSample *= g_frame.LensRadius;
            }

            float3 pos_i = Math::WorldPosFromScreenSpace2(posSS_i, renderDim, depth_i, 
                g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.PrevCameraJitter, 
                g_frame.PrevView[0].xyz, g_frame.PrevView[1].xyz, g_frame.PrevView[2].xyz, 
                g_frame.DoF, lensSample, g_frame.FocusDepth, origin_i);

            float tolerance = MAX_PLANE_DIST_REUSE * (g_frame.DoF ? 10 : 1);
            bool valid = PlaneHeuristic(pos_i, normal, pos, linearDepth, tolerance);          
            valid = valid && (abs(mr_i.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE);

            if (!valid)
                continue;

            samplePos[k] = pos_i;
            sampleOrigin[k] = origin_i;
            samplePosSS[k] = (int16_t2)posSS_i;
            sampleMetallic[k] = flags_i.metallic;
            sampleRoughness[k] = mr_i.y;
            sampleTransmissive[k] = flags_i.transmissive;
            sampleEta_mat[k] = DEFAULT_ETA_MAT;

            if(flags_i.transmissive)
            {
                float ior = g_prevIOR[posSS_i];
                sampleEta_mat[k] = GBuffer::DecodeIOR(ior);
            }

            k++;
        }
    }

    pairwiseMIS.k = k;

    for (int i = 0; i < k; i++)
    {
        const float3 sampleNormal = Math::DecodeUnitVector(g_prevNormal[samplePosSS[i]]);
        const float3 sampleBaseColor = g_prevBaseColor[samplePosSS[i]].rgb;

        const float3 wo_i = normalize(sampleOrigin[i] - samplePos[i]);
        BSDF::ShadingData surface_i = BSDF::ShadingData::Init(sampleNormal, wo_i,
            sampleMetallic[i], sampleRoughness[i], sampleBaseColor, ETA_AIR,
            sampleEta_mat[i], sampleTransmissive[i]);

        SkyDI_Util::Reservoir neighbor = SkyDI_Util::PartialReadReservoir_Reuse(samplePosSS[i], 
            g_local.PrevReservoir_A_DescHeapIdx);
        const float neighborWSum = SkyDI_Util::PartialReadReservoir_WSum(samplePosSS[i], 
            prevReservoir_B_DescHeapIdx);

        pairwiseMIS.Stream(r, pos, normal, surface, neighbor, samplePos[i], 
            sampleNormal, neighborWSum, surface_i, rng);
    }

    pairwiseMIS.End(r, rng);
    r = pairwiseMIS.r_s;
}

SkyDI_Util::Reservoir EstimateDirectLighting(uint2 DTid, float3 pos, float3 normal, float linearDepth, 
    bool metallic, float roughness, float3 baseColor, BSDF::ShadingData surface, inout RNG rng)
{
    SkyDI_Util::Reservoir r = RIS_InitialCandidates(DTid, pos, normal, metallic, roughness,
        surface, rng);

    // TODO use virtual motion vector for specular surfaces
    const bool resample = roughness > g_local.MinRoughnessResample;
    
    if (IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::TEMPORAL_RESAMPLE) && resample) 
    {
        TemporalCandidate candidate = FindTemporalCandidate(DTid, pos, normal, linearDepth, 
            metallic, roughness, surface, rng);

        if(candidate.valid)
            TemporalResample(candidate, pos, normal, metallic, surface, r, rng);
    }

    float m_max = 1 + smoothstep(0, 0.6, roughness * roughness) * g_local.M_max;
    
    SkyDI_Util::WriteReservoir(DTid, r, g_local.CurrReservoir_A_DescHeapIdx,
            g_local.CurrReservoir_B_DescHeapIdx, (half)m_max);

    if(IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::SPATIAL_RESAMPLE) && resample)
    {
        SpatialResample(DTid, NUM_SPATIAL_SAMPLES, SPATIAL_SEARCH_RADIUS, pos, normal, 
            linearDepth, roughness, surface, g_local.PrevReservoir_A_DescHeapIdx, 
            g_local.PrevReservoir_B_DescHeapIdx, r, rng);
    }

    return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_TEMPORAL_GROUP_DIM_X, SKY_DI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(SKY_DI_TEMPORAL_GROUP_DIM_X, SKY_DI_TEMPORAL_GROUP_DIM_Y),
        g_local.DispatchDimX, 
        SKY_DI_TEMPORAL_TILE_WIDTH, 
        SKY_DI_TEMPORAL_LOG2_TILE_WIDTH, 
        g_local.NumGroupsInTile,
        swizzledGid);
#else
    const uint2 swizzledDTid = DTid.xy;
    const uint2 swizzledGid = Gid.xy;
#endif

    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    if (flags.invalid || flags.emissive)
        return;

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float z_view = g_depth[swizzledDTid];
    
    float2 lensSample = 0;
    float3 origin = g_frame.CameraPos;
    if(g_frame.DoF)
    {
        RNG rngDoF = RNG::Init(RNG::PCG3d(swizzledDTid.xyx).zy, g_frame.FrameNum);
        lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
        lensSample *= g_frame.LensRadius;
    }

    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 pos = Math::WorldPosFromScreenSpace2(swizzledDTid, renderDim, z_view, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
        g_frame.DoF, lensSample, g_frame.FocusDepth, origin);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid]);

    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_baseColor[swizzledDTid].rgb;

    float eta_curr = ETA_AIR;
    float eta_next = DEFAULT_ETA_MAT;

    if(flags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[swizzledDTid];
        eta_next = GBuffer::DecodeIOR(ior);
    }

    const float3 wo = normalize(origin - pos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, mr.y, baseColor, 
        eta_curr, eta_next, flags.transmissive);

    RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum);

    SkyDI_Util::Reservoir r = EstimateDirectLighting(swizzledDTid, pos, normal, z_view, 
        flags.metallic, mr.y, baseColor, surface, rng);

    if(IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::DENOISE))
    {
        // split into diffuse & specular, so they can be denoised separately
        surface.SetWi_Refl(r.wi, normal);
        float3 fr = surface.Fresnel();

        // demodulate base color
        float3 f_d = (1.0f - fr) * (1.0f - flags.metallic) * surface.ndotwi * ONE_OVER_PI;
        
        // demodulate Fresnel for metallic surfaces to preserve texture detail
        float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
        float3 f_s = 0;

        if(flags.metallic)
        {
            if(surface.specular)
            {
                f_s = (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);
            }
            else
            {
                float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
                float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
                float G2Div_ndotwi_ndotwo = BSDF::SmithHeightCorrelatedG2_GGX_Opt<1>(alphaSq, surface.ndotwi, surface.ndotwo);
                f_s = NDF * G2Div_ndotwi_ndotwo * surface.ndotwi;
            }
        }
        else
            f_s = BSDF::EvalGGXMicrofacetBRDF(surface, fr);

        float3 Li_d = r.Le * f_d * r.W;
        float3 Li_s = r.Le * f_s * r.W;
        float3 wh = normalize(surface.wo + r.wi);
        float whdotwo = saturate(dot(wh, surface.wo));
        float tmp = 1.0f - whdotwo;
        float tmpSq = tmp * tmp;
        uint tmpU = asuint(tmpSq * tmpSq * tmp);
        half2 encoded = half2(asfloat16(uint16_t(tmpU & 0xffff)), asfloat16(uint16_t(tmpU >> 16)));

        RWTexture2D<float4> g_colorA = ResourceDescriptorHeap[g_local.ColorAUavDescHeapIdx];
        RWTexture2D<half4> g_colorB = ResourceDescriptorHeap[g_local.ColorBUavDescHeapIdx];

        g_colorA[swizzledDTid] = float4(Li_s, Li_d.r);
        g_colorB[swizzledDTid] = half4(Li_d.gb, encoded);
    }
    else
    {
        float3 Li = r.Target * r.W;
        Li = any(isnan(Li)) ? 0 : Li;
        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + Li;
        }
        else
            g_final[swizzledDTid].rgb = Li;
    }
}