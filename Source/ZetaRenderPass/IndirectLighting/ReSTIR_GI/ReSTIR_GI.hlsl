#ifndef NEE_EMISSIVE
#define NEE_EMISSIVE 0
#endif

#include "Resampling.hlsli"
#include "../../Common/Common.hlsli"

#define THREAD_GROUP_SWIZZLING 1

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_GI> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t1);
StructuredBuffer<Vertex> g_vertices : register(t2);
StructuredBuffer<uint> g_indices : register(t3);
StructuredBuffer<Material> g_materials : register(t4);
#if NEE_EMISSIVE == 1
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t5);
StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(t6);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t7);
StructuredBuffer<RT::VoxelSample> g_lvg : register(t8);
#endif

//--------------------------------------------------------------------------------------
// Utility Functions
//--------------------------------------------------------------------------------------

ReSTIR_Util::Globals InitGlobals(bool transmissive)
{
    ReSTIR_Util::Globals globals;
    globals.bvh = g_bvh;
    globals.frameMeshData = g_frameMeshData;
    globals.vertices = g_vertices;
    globals.indices = g_indices;
    globals.materials = g_materials;
    globals.maxNumBounces = transmissive ? (uint16_t)g_local.MaxGlossyTrBounces :
        (uint16_t)g_local.MaxNonTrBounces;
    globals.russianRoulette = IS_CB_FLAG_SET(CB_IND_FLAGS::RUSSIAN_ROULETTE);

#if NEE_EMISSIVE == 1
    globals.emissives = g_emissives;
    globals.sampleSets = g_sampleSets;
    globals.aliasTable = g_aliasTable;
    globals.sampleSetSize = (uint16_t)(g_local.SampleSetSize_NumSampleSets & 0xffff);
    globals.lvg = g_lvg;
    globals.gridDim = uint16_t3(g_local.GridDim_xy & 0xffff, g_local.GridDim_xy >> 16, (uint16_t)g_local.GridDim_z);
    globals.extents = asfloat16(uint16_t3(g_local.Extents_xy & 0xffff, g_local.Extents_xy >> 16, 
            g_local.Extents_z_Offset_y & 0xffff));
    globals.offset_y = asfloat16(uint16_t(g_local.Extents_z_Offset_y >> 16));

#endif

    return globals;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_GI_TEMPORAL_GROUP_DIM_X, RESTIR_GI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint16_t2 swizzledGid;

    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, 
        uint16_t2(RESTIR_GI_TEMPORAL_GROUP_DIM_X, RESTIR_GI_TEMPORAL_GROUP_DIM_Y),
        uint16_t(g_local.DispatchDimX_NumGroupsInTile & 0xffff), 
        RESTIR_GI_TEMPORAL_TILE_WIDTH, 
        RESTIR_GI_TEMPORAL_LOG2_TILE_WIDTH, 
        uint16_t(g_local.DispatchDimX_NumGroupsInTile >> 16),
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
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid.xy]);

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

    // Per-group RNG
    RNG rngGroup = RNG::Init(swizzledGid ^ 61, g_frame.FrameNum);
    // Per-thread RNG
    RNG rngThread = RNG::Init(uint2(swizzledDTid.x ^ 511, swizzledDTid.y ^ 31), g_frame.FrameNum);

    ReSTIR_Util::Globals globals = InitGlobals(flags.transmissive);

    RGI_Util::Reservoir r = RGI_Util::EstimateIndirectLighting(swizzledDTid, origin, lensSample,
        pos, normal, mr.y, eta_next, z_view,  surface, g_frame, g_local, globals, rngThread, rngGroup);

    if (IS_CB_FLAG_SET(CB_IND_FLAGS::DENOISE))
    {
        // Split into diffuse & glossy reflection, so they can be denoised separately
        float3 wi = normalize(r.pos - pos);
        surface.SetWi_Refl(wi, normal);
        float3 fr = surface.Fresnel();

        // Demodulate base color
        float3 f_d = (1.0f - fr) * (1.0f - flags.metallic) * surface.ndotwi * ONE_OVER_PI;
        float3 f_s = 0;

        // Demodulate Fresnel for metallic surfaces to preserve texture detail
        float alphaSq = surface.alpha * surface.alpha;
        if(flags.metallic)
        {
            if(surface.specular)
            {
                f_s = (surface.ndotwh >= MIN_N_DOT_H_SPECULAR);
            }
            else
            {
                float NDF = BSDF::GGX(surface.ndotwh, alphaSq);
                float G2Div_ndotwi_ndotwo = BSDF::SmithHeightCorrelatedG2_GGX_Opt<1>(alphaSq, surface.ndotwi, surface.ndotwo);
                f_s = NDF * G2Div_ndotwi_ndotwo * surface.ndotwi;
            }
        }
        else
            f_s = BSDF::EvalGGXMicrofacetBRDF(surface, fr);

        float3 Li_d = r.Lo * f_d * r.W;
        float3 Li_s = r.Lo * f_s * r.W;
        float3 wh = normalize(surface.wo + wi);
        float whdotwo = saturate(dot(wh, surface.wo));
        float tmp = 1.0f - whdotwo;
        float tmpSq = tmp * tmp;
        uint tmpU = asuint(tmpSq * tmpSq * tmp);
        half2 encoded = half2(asfloat16(uint16_t(tmpU & 0xffff)), asfloat16(uint16_t(tmpU >> 16)));

        RWTexture2D<float4> g_colorA = ResourceDescriptorHeap[g_local.FinalOrColorAUavDescHeapIdx];
        RWTexture2D<half4> g_colorB = ResourceDescriptorHeap[g_local.ColorBUavDescHeapIdx];

        g_colorA[swizzledDTid] = float4(Li_s, Li_d.r);
        g_colorB[swizzledDTid] = half4(Li_d.gb, encoded);
    }
    else
    {
        float3 li = r.target_z * r.W;
        li = any(isnan(li)) ? 0 : li;

        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalOrColorAUavDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + li;
        }
        else
            g_final[swizzledDTid].rgb = li;
    }
}
