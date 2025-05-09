#ifndef NEE_EMISSIVE
#define NEE_EMISSIVE 0
#endif

#include "../../Common/Common.hlsli"
#include "Util.hlsli"

#define THREAD_GROUP_SWIZZLING 1

using namespace RtRayQuery;
using namespace ReSTIR_Util;
using namespace RPT_Util;

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_PT_PathTrace> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t1);
StructuredBuffer<Vertex> g_vertices : register(t2);
StructuredBuffer<uint> g_indices : register(t3);
StructuredBuffer<Material> g_materials : register(t4);
#if NEE_EMISSIVE == 1
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t5);
StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(t6);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t7);
#endif

//--------------------------------------------------------------------------------------
// Utility Functions
//--------------------------------------------------------------------------------------

// Data needed from previous hit to decide if reconnection is possible
struct PrevHit
{
    static PrevHit Init(float alpha, BSDF::LOBE lobe_prev, float3 wi, float pdf)
    {
        PrevHit ret;
        ret.alpha_lobe = alpha;
        ret.lobe = lobe_prev;
        ret.wi = wi;
        ret.pdf = pdf;

        return ret;
    }

    float alpha_lobe;
    float3 wi;
    float pdf;
    BSDF::LOBE lobe;
};

ReSTIR_Util::Globals InitGlobals(bool transmissive)
{
    ReSTIR_Util::Globals globals;
    globals.bvh = g_bvh;
    globals.frameMeshData = g_frameMeshData;
    globals.vertices = g_vertices;
    globals.indices = g_indices;
    globals.materials = g_materials;
    uint maxNonTrBounces = g_local.Packed & 0xf;
    uint maxGlossyTrBounces = (g_local.Packed >> PACKED_INDEX::NUM_GLOSSY_BOUNCES) & 0xf;
    globals.maxNumBounces = transmissive ? (uint16_t)maxGlossyTrBounces :
        (uint16_t)maxNonTrBounces;
#if NEE_EMISSIVE == 1
    globals.emissives = g_emissives;
    globals.sampleSets = g_sampleSets;
    globals.aliasTable = g_aliasTable;
    globals.sampleSetSize = (uint16_t)(g_local.SampleSetSize_NumSampleSets & 0xffff);
#endif

    return globals;
}

void MaybeSetCase2OrCase3(int16 pathVertex, float3 pos, float3 normal, float t, 
    uint ID, uint meshIdx, BSDF::ShadingData surface, PrevHit prevHit, DirectLightingEstimate ls, 
    uint seed_nee, inout Reconnection rc)
{
    // Possibly update reconnection vertex (Case 2)
    const float alpha_lobe_direct = BSDF::LobeAlpha(surface, ls.lobe);

    if(rc.Empty() && RPT_Util::CanReconnect(prevHit.alpha_lobe, alpha_lobe_direct, 
        0, pos, prevHit.lobe, ls.lobe, g_local.Alpha_min))
    {
        rc.SetCase2(pathVertex, pos, t, normal, ID, meshIdx,
            prevHit.wi, prevHit.lobe, prevHit.pdf, 
            ls.wi, ls.lobe, ls.pdf_solidAngle, ls.lt, ls.pdf_light, 
            ls.le, seed_nee, ls.dwdA);
    }

    // Possibly update reconnection vertex Case 3) -- If some light source is reached 
    // with no earlier reconnection found, then the light sample could be the reconnection 
    // vertex assuming the prior vertex is sufficiently rough.
    if(rc.Empty() && (alpha_lobe_direct >= g_local.Alpha_min))
    {
        rc.SetCase3(pathVertex + (int16_t)1, ls.pos, ls.lt, ls.lobe, 
            ls.ID, ls.le, ls.normal, ls.pdf_solidAngle, ls.pdf_light, 
            ls.dwdA, ls.wi, ls.twoSided, seed_nee);
    }
}

template<bool Emissive>
void EstimateDirectAndUpdateRC(int16 pathVertex, float3 pos, Hit hitInfo, 
    BSDF::ShadingData surface, PrevHit prevHit, float3 throughput, float3 throughput_k, 
    uint sampleSetIdx, Globals globals, inout float3 li, inout BSDF::BSDFSample bsdfSample, 
    out Hit_Emissive nextHit, inout Reconnection rc, inout Reservoir r, inout RNG rngNEE, 
    inout RNG rngReplay)
{
    // Path resampling:
    //  - target = (Le * \prod f * g) / p
    //  - p_source = 1

    if(Emissive)
    {
        BSDF::BSDFSample nextBsdfSample;
        int16 nextBounce = pathVertex - (int16)1;

        // BSDF sampling
        DirectLightingEstimate ls_b = RPT_Util::NEE_Bsdf(pos, hitInfo.normal, surface, 
            nextBounce, globals, g_frame.EmissiveMapsDescHeapOffset, nextBsdfSample, 
            nextHit, rngReplay);

        if(nextHit.HitWasEmissive())
        {
            const float3 fOverPdf = throughput * ls_b.ld;
            li += fOverPdf;

            // Case 1: For path x_0, ..., x_k, x_{k + 1}, ..., L is the radiance reflected from 
            // x_{k + 1} towards x_k. If connection becomes case 2 or case 3, it is overwritten.
            rc.L = half3(ls_b.ld * throughput_k);

            MaybeSetCase2OrCase3(pathVertex, pos, hitInfo.normal, hitInfo.t, hitInfo.ID, 
                hitInfo.meshIdx, surface, prevHit, ls_b, /*unused*/ 0, rc);

            float risWeight = Math::Luminance(fOverPdf);
            r.Update(risWeight, fOverPdf, rc, rngNEE);
        }

        // Light sampling
        const bool specular = surface.GlossSpecular() && (surface.metallic || surface.specTr) && 
            (!surface.Coated() || surface.CoatSpecular());

        if(!specular)
        {
            const uint seed_nee = rngNEE.State;
            DirectLightingEstimate ls = RPT_Util::NEE_Emissive(pos, hitInfo.normal, 
                surface, sampleSetIdx, g_frame.NumEmissiveTriangles, nextBounce, globals, 
                g_frame.EmissiveMapsDescHeapOffset, rngNEE);

            const float3 fOverPdf = throughput * ls.ld;
            li += fOverPdf;

            // Different (sub)path
            if(rc.IsCase2() || rc.IsCase3())
                rc.Clear();

            // Case 1: For path x_0, ..., x_k, x_{k + 1}, ..., L is the radiance reflected from 
            // x_{k + 1} towards x_k. If connection becomes case 2 or case 3, it is overwritten.
            rc.L = half3(ls.ld * throughput_k);

            MaybeSetCase2OrCase3(pathVertex, pos, hitInfo.normal, hitInfo.t, hitInfo.ID, 
                hitInfo.meshIdx, surface, prevHit, ls, seed_nee, rc);

            float risWeight = Math::Luminance(fOverPdf);
            r.Update(risWeight, fOverPdf, rc, rngNEE);
        }

        // Use BSDF sample for next path vertex
        bsdfSample = nextBsdfSample;
    }
    else
    {
        const uint seed_nee = rngNEE.State;
        DirectLightingEstimate ls = RPT_Util::NEE_NonEmissive(pos, hitInfo.normal, surface, 
            g_frame, globals.bvh, rngNEE);

        const float3 fOverPdf = throughput * ls.ld;
        li += fOverPdf;

        // Case 1: For path x_0, ..., x_k, x_{k + 1}, ..., L is the radiance reflected from 
        // x_{k + 1} towards x_k. If connection becomes case 2 or case 3, it is overwritten.
        rc.L = half3(ls.ld * throughput_k);

        MaybeSetCase2OrCase3(pathVertex, pos, hitInfo.normal, hitInfo.t, hitInfo.ID, 
            hitInfo.meshIdx, surface, prevHit, ls, seed_nee, rc);

        float risWeight = Math::Luminance(fOverPdf);
        r.Update(risWeight, fOverPdf, rc, rngNEE);
    }
}

RPT_Util::Reservoir PathTrace(float3 pos, float3 normal, float ior, BSDF::ShadingData surface, 
    uint sampleSetIdx, BSDF::BSDFSample bsdfSample, RT::RayDifferentials rd, 
    Globals globals, inout RNG rngReplay, inout RNG rngThread, inout RNG rngGroup, 
    out float3 li)
{
    RPT_Util::Reconnection reconnection = RPT_Util::Reconnection::Init();
    RPT_Util::Reservoir r = RPT_Util::Reservoir::Init();
    li = 0.0;
    int16 bounce = 0;
    float3 throughput = bsdfSample.bsdfOverPdf;
    PrevHit prevHit = PrevHit::Init(BSDF::LobeAlpha(surface, bsdfSample.lobe), 
        bsdfSample.lobe, bsdfSample.wi, bsdfSample.pdf);

    // If ray was refracted, current IOR changes to that of hit material, otherwise
    // current medium continues to be air
    float eta_curr = dot(normal, bsdfSample.wi) < 0 ? ior : ETA_AIR;
    // Product of f / p terms from x_{k + 1} onwards. Needed for case 1 connections.
    float3 throughput_k = 1;
    // Note: skip the first bounce for a milder impacet. May have to change in the future.
    // bool anyGlossyBounces = bsdfSample.lobe != BSDF::LOBE::DIFFUSE_R;
    // bool anyGlossyBounces = false;
    bool inTranslucentMedium = eta_curr != ETA_AIR;
    SamplerState samp = SamplerDescriptorHeap[g_local.TexFilterDescHeapIdx];

#if NEE_EMISSIVE == 0
    // unused
    Hit_Emissive nextHit;
#else
    // Use the same BSDF ray used for direct lighting at previous path vertex
    Hit_Emissive nextHit = Hit_Emissive::FindClosest(pos, normal, bsdfSample.wi, 
        globals.bvh, g_frameMeshData, surface.Transmissive());
#endif

    while(true)
    {
        // Skip first two vertices (camera and primary)
        const int16 pathVertex = bounce + (int16)2;

#if NEE_EMISSIVE == 0
        Hit hitInfo = Hit::FindClosest<true, true>(pos, normal, bsdfSample.wi, g_bvh, g_frameMeshData, 
            g_vertices, g_indices, surface.Transmissive());
#else
        // Use the same BSDF ray used for direct lighting at previous path vertex
        RtRayQuery::Hit hitInfo = nextHit.ToHitInfo<true>(bsdfSample.wi, g_frameMeshData, 
            g_vertices, g_indices);
#endif

        if(!hitInfo.hit)
            break;

        float3 newPos = mad(hitInfo.t, bsdfSample.wi, pos);
        float3 dpdx;
        float3 dpdy;
        rd.dpdx_dpdy(newPos, hitInfo.normal, dpdx, dpdy);
        rd.ComputeUVDifferentials(dpdx, dpdy, hitInfo.triDiffs.dpdu, hitInfo.triDiffs.dpdv);

        // Fetch material at new vertex
        float eta_next;
        if(!RtRayQuery::GetMaterialData(-bsdfSample.wi, g_materials, g_frame, eta_curr, 
            rd.uv_grads, hitInfo, surface, eta_next, samp))
        {
           break;
        }

        // if(IS_CB_FLAG_SET(CB_IND_FLAGS::PATH_REGULARIZATION) && anyGlossyBounces)
        //     surface.Regularize();

        // Update vertex
        pos = newPos;
        normal = hitInfo.normal;
        const float prevBsdfSamplePdf = bsdfSample.pdf;
        const BSDF::LOBE prevBsdfSampleLobe = bsdfSample.lobe;
        float3 tr = 1;

        // Beer's law
        if(inTranslucentMedium && (surface.trDepth > 0))
        {
            float3 extCoeff = -log(surface.baseColor_Fr0_TrCol) / surface.trDepth;
            tr = exp(-hitInfo.t * extCoeff);
            throughput *= tr;
        }

        // Direct lighting
        EstimateDirectAndUpdateRC<NEE_EMISSIVE>(pathVertex, pos, hitInfo, surface, prevHit, 
            throughput, throughput_k, sampleSetIdx, globals, li, bsdfSample, nextHit, 
            reconnection, r, rngThread, rngReplay);

        // Remaining code can be skipped in the last iteration
        if(bounce >= (globals.maxNumBounces - 1))
            break;

        // Once one vertex x_k satisfying the reconnection conditions is found (with x_k and x_{k + 1} 
        // not on a light source), all the subsequent paths share x_k as their reconnection vertex 
        // as paths are built incrementally.
        if(reconnection.IsCase2() || reconnection.IsCase3())
            reconnection.Clear();

        // Move to next path vertex
        bounce++;

        // Russian Roulette
        if(IS_CB_FLAG_SET(CB_IND_FLAGS::RUSSIAN_ROULETTE) && (bounce >= MIN_NUM_BOUNCES_RUSSIAN_ROULETTE))
        {
            // Test against maximum throughput across the wave
            float waveThroughput = WaveActiveMax(Math::Luminance(throughput));
            if(waveThroughput < 1)
            {
                float p_terminate = max(0.05, 1 - waveThroughput);
                if(rngGroup.Uniform() < p_terminate)
                    break;
                
                throughput /= (1 - p_terminate);
                throughput_k /= (reconnection.k <= bounce) ? (1 - p_terminate) : 1;
            }
        }

#if NEE_EMISSIVE == 0
        // Sample BSDF to generate new direction
        bsdfSample = BSDF::BSDFSample::Init();

        if(bounce < globals.maxNumBounces)
            bsdfSample = BSDF::SampleBSDF(normal, surface, rngReplay);
#endif

        // Terminate early as extending this path won't contribute anything
        if(dot(bsdfSample.bsdfOverPdf, bsdfSample.bsdfOverPdf) == 0)
            break;

        // With x_k as reconnection vertex, now we know w_k towards the non-light vertex after it
        const float alpha_lobe = BSDF::LobeAlpha(surface, bsdfSample.lobe);

        // Possibly update reconnection vertex (x_{k + 1} on a non-light vertex)
        if(reconnection.Empty() && RPT_Util::CanReconnect(prevHit.alpha_lobe, alpha_lobe, 
            0, 0, prevHit.lobe, bsdfSample.lobe, g_local.Alpha_min))
        {
            reconnection.SetCase1(pathVertex, pos, hitInfo.t, hitInfo.normal, hitInfo.ID,
                hitInfo.meshIdx, -surface.wo, prevBsdfSampleLobe, prevBsdfSamplePdf,
                bsdfSample.wi, bsdfSample.lobe, bsdfSample.pdf);

            throughput_k = 1;
        }

        // Update path throughput starting from vertex after reconnection 
        if(reconnection.k <= bounce)
            throughput_k *= bsdfSample.bsdfOverPdf * tr;

        bool transmitted = dot(normal, bsdfSample.wi) < 0;
        throughput *= bsdfSample.bsdfOverPdf;
        // anyGlossyBounces = anyGlossyBounces || (bsdfSample.lobe != BSDF::LOBE::DIFFUSE_R);

        eta_curr = transmitted ? (eta_curr == ETA_AIR ? eta_next : ETA_AIR) : eta_curr;
        inTranslucentMedium = eta_curr != ETA_AIR;
        prevHit.alpha_lobe = alpha_lobe;
        prevHit.lobe = bsdfSample.lobe;
        prevHit.wi = bsdfSample.wi;
        prevHit.pdf = bsdfSample.pdf;

        // Given the hit point and new sample direction, update origin and direction of 
        // ray differentials accordingly
        rd.UpdateRays(pos, normal, bsdfSample.wi, surface.wo, hitInfo.triDiffs, 
            dpdx, dpdy, transmitted, surface.eta);
    }

    return r;
}

RPT_Util::Reservoir RIS_InitialCandidates(uint2 DTid, uint2 Gid, float3 origin, 
    float2 lensSample, float3 pos, float3 normal, float ior, BSDF::ShadingData surface, 
    out float3 li)
{
    // Use a different index than ReSTIR DI
    RNG rngGroup = RNG::Init(Gid, g_frame.FrameNum, 1);

    const uint3 state = RNG::PCG3d(uint3(DTid, g_frame.FrameNum));
    // Only used for BSDF sampling
    RNG rngReplay = RNG::Init(state.x);
    // For anything else
    RNG rngThread = RNG::Init(state.y);

    Globals globals = InitGlobals(surface.specTr);
    BSDF::BSDFSample bsdfSample = BSDF::SampleBSDF(normal, surface, rngReplay);
    if(dot(bsdfSample.bsdfOverPdf, bsdfSample.bsdfOverPdf) == 0)
    {
        li = 0;
        return RPT_Util::Reservoir::Init();
    }

    GBUFFER_TRI_DIFF_GEO_A g_triA = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_A];
    GBUFFER_TRI_DIFF_GEO_B g_triB = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::TRI_DIFF_GEO_B];
    const uint4 packed_a = g_triA[DTid];
    const uint2 packed_b = g_triB[DTid];

    Math::TriDifferentials triDiffs = Math::TriDifferentials::Unpack(packed_a, packed_b);
    float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

    RT::RayDifferentials rd = RT::RayDifferentials::Init(DTid, renderDim, 
        g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
        g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
        g_frame.CurrView[2].xyz, g_frame.DoF, g_frame.FocusDepth, 
        lensSample, origin);

    float3 dpdx;
    float3 dpdy;
    rd.dpdx_dpdy(pos, normal, dpdx, dpdy);
    rd.ComputeUVDifferentials(dpdx, dpdy, triDiffs.dpdu, triDiffs.dpdv);

    rd.UpdateRays(pos, normal, bsdfSample.wi, surface.wo, triDiffs, 
        dpdx, dpdy, dot(bsdfSample.wi, normal) < 0, surface.eta);

    // Use the same sample set for all threads in this group
    const uint sampleSetIdx = NEE_EMISSIVE == 1 ? 
        rngGroup.UniformUintBounded_Faster(g_local.SampleSetSize_NumSampleSets >> 16) : 
        0;

    RPT_Util::Reservoir r = PathTrace(pos, normal, ior, surface, sampleSetIdx, 
        bsdfSample, rd, globals, rngReplay, rngThread, rngGroup, li);

    // Remember RNG seed for replay
    r.rc.seed_replay = state.x;

    return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_PT_PATH_TRACE_GROUP_DIM_X, RESTIR_PT_PATH_TRACE_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint2 swizzledGid;
    const uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint2(RESTIR_PT_PATH_TRACE_GROUP_DIM_X, RESTIR_PT_PATH_TRACE_GROUP_DIM_Y),
        g_local.DispatchDimX_NumGroupsInTile & 0xffff, 
        RESTIR_PT_TILE_WIDTH, 
        RESTIR_PT_LOG2_TILE_WIDTH,
        g_local.DispatchDimX_NumGroupsInTile >> 16,
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
    
    RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.Final];

    if (flags.invalid || flags.emissive)
    {
        if(!g_frame.Accumulate || !g_frame.CameraStatic)
            g_final[swizzledDTid].rgb = 0;
    
        return;
    }

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        GBUFFER_OFFSET::DEPTH];
    const float z_view = g_depth[swizzledDTid];

    // Use the same lens sample that was used for this pixel's camera ray
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
    const float4 baseColor = flags.subsurface ? g_baseColor[swizzledDTid] :
        float4(g_baseColor[swizzledDTid].rgb, 0);

    float eta_curr = ETA_AIR;
    float eta_next = DEFAULT_ETA_MAT;

    if(flags.transmissive)
    {
        GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::IOR];

        float ior = g_ior[swizzledDTid];
        eta_next = GBuffer::DecodeIOR(ior);
    }

    float coat_weight = 0;
    float3 coat_color = 0.0f;
    float coat_roughness = 0;
    float coat_ior = DEFAULT_ETA_COAT;

    if(flags.coated)
    {
        GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::COAT];
        uint3 packed = g_coat[swizzledDTid].xyz;

        GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
        coat_weight = coat.weight;
        coat_color = coat.color;
        coat_roughness = coat.roughness;
        coat_ior = coat.ior;
    }

    const float3 wo = normalize(origin - pos);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, mr.y, 
        baseColor.xyz, eta_curr, eta_next, flags.transmissive, flags.trDepthGt0, (half)baseColor.w,
        coat_weight, coat_color, coat_roughness, coat_ior);

    float3 li;
    RPT_Util::Reservoir r = RIS_InitialCandidates(swizzledDTid, swizzledGid, origin, lensSample,
        pos, normal, eta_next, surface, li);

    // Since p_source = 1, weight = |target|
    float targetLum = Math::Luminance(r.target);
    // We have:
    //      w_sum = \sum m_1 f_1 / p_1 + ... + m_n f_n / p_n
    //      W = w_sum / (m_z f_z / p_z) with z \in {1, ..., n}.
    // Given above, W must be >= 1, but due to round-off errors, it
    // could become < 1.
    r.W = targetLum > 0 ? max(r.w_sum / targetLum, 1.0f) : 0;

    if(IS_CB_FLAG_SET(CB_IND_FLAGS::TEMPORAL_RESAMPLE) || 
        IS_CB_FLAG_SET(CB_IND_FLAGS::RESET_TEMPORAL_TEXTURES))
    {
        r.Write<NEE_EMISSIVE>(swizzledDTid, g_local.Reservoir_A_DescHeapIdx, 
            g_local.Reservoir_A_DescHeapIdx + 1, g_local.Reservoir_A_DescHeapIdx + 2, 
            g_local.Reservoir_A_DescHeapIdx + 3, g_local.Reservoir_A_DescHeapIdx + 4,
            g_local.Reservoir_A_DescHeapIdx + 5, g_local.Reservoir_A_DescHeapIdx + 6);
    }

    if(IS_CB_FLAG_SET(CB_IND_FLAGS::TEMPORAL_RESAMPLE))
        r.WriteTarget(swizzledDTid, g_local.TargetDescHeapIdx);
    else
    {
        RPT_Util::DebugColor(r.rc, g_local.Packed, li);
        li = any(isnan(li)) ? 0 : li;

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + li;
        }
        else
            g_final[swizzledDTid].rgb = li;
    }
}
