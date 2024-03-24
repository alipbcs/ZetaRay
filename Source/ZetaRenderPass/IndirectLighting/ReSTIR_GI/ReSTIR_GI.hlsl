#include "ReSTIR_GI_Resampling.hlsli"
#include "../../Common/Common.hlsli"
#include "../../../ZetaCore/Core/Material.h"

#define THREAD_GROUP_SWIZZLING 1

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_ReSTIR_GI_SpatioTemporal> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t1);
StructuredBuffer<Vertex> g_vertices : register(t2);
StructuredBuffer<uint> g_indices : register(t3);
StructuredBuffer<Material> g_materials : register(t4);
StructuredBuffer<RT::EmissiveTriangle> g_emissives : register(t5);
StructuredBuffer<RT::PresampledEmissiveTriangle> g_sampleSets : register(t6);
StructuredBuffer<RT::EmissiveLumenAliasTableEntry> g_aliasTable : register(t7);
StructuredBuffer<RT::VoxelSample> g_lvg : register(t8);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(RESTIR_GI_TEMPORAL_GROUP_DIM_X, RESTIR_GI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
    uint16_t2 swizzledGid;

    // Swizzle thread groups for better L2-cache behavior
    // Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
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

    GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
    const float z_view = g_depth[swizzledDTid];
    
    if (z_view == FLT_MAX)
        return;

    // Reconstruct position from depth
    const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    const float3 posW = Math::WorldPosFromScreenSpace(swizzledDTid,
        renderDim,
        z_view,
        g_frame.TanHalfFOV,
        g_frame.AspectRatio,
        g_frame.CurrViewInv,
        g_frame.CurrCameraJitter);

    GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
    const float3 normal = Math::DecodeUnitVector(g_normal[swizzledDTid.xy]);

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[swizzledDTid];
    bool isMetallic;
    bool isTransmissive;
    bool isEmissive;
    GBuffer::DecodeMetallic(mr.x, isMetallic, isTransmissive, isEmissive);

    if (isEmissive)
        return;

    GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::BASE_COLOR];
    const float3 baseColor = g_baseColor[swizzledDTid].rgb;

    float tr = DEFAULT_SPECULAR_TRANSMISSION;
    float eta_t = DEFAULT_ETA_T;
    float eta_i = DEFAULT_ETA_I;

    if(isTransmissive)
    {
        GBUFFER_TRANSMISSION g_transmission = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::TRANSMISSION];

        float2 tr_ior = g_transmission[swizzledDTid];
        tr = tr_ior.x;
        eta_i = GBuffer::DecodeIOR(tr_ior.y);
    }

    const float3 wo = normalize(g_frame.CameraPos - posW);
    BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, isMetallic, mr.y, baseColor, 
        eta_i, eta_t, tr);

    Texture2D<float> g_curvature = ResourceDescriptorHeap[g_local.CurvatureDescHeapIdx];
    float localCurvature = g_curvature[swizzledDTid];

    // Per-group RNG
    RNG rngGroup = RNG::Init(swizzledGid ^ 61, g_frame.FrameNum);
    // Per-thread RNG
    RNG rngThread = RNG::Init(uint2(swizzledDTid.x ^ 511, swizzledDTid.y ^ 31), g_frame.FrameNum);

    RGI_Util::Reservoir r = RGI_Util::EstimateIndirectLighting(swizzledDTid, posW, normal, mr.y, 
        eta_i, z_view, surface, localCurvature, g_frame, g_local, g_bvh, g_frameMeshData, g_vertices, 
        g_indices, g_materials, g_emissives, g_sampleSets, g_aliasTable, g_lvg, rngThread, rngGroup);

    if (IS_CB_FLAG_SET(CB_IND_FLAGS::DENOISE))
    {
        // Split into diffuse & glossy reflection, so they can be denoised separately
        float3 wi = normalize(r.pos - posW);
        surface.SetWi_Refl(wi, normal);
        float3 fr = surface.Fresnel();

        // Demodulate base color
        float3 f_d = (1.0f - fr) * (1.0f - isMetallic) * surface.ndotwi * ONE_OVER_PI;
        float3 f_s = 0;

        // Demodulate Fresnel for metallic surfaces to preserve texture detail
        float alphaSq = surface.alpha * surface.alpha;
        if(isMetallic)
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
            f_s = BSDF::MicrofacetBRDFGGXSmith(surface, fr);

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
        float3 Li = r.target_z * r.W;
        Li = any(isnan(Li)) ? 0 : Li;

        RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalOrColorAUavDescHeapIdx];

        if(g_frame.Accumulate && g_frame.CameraStatic)
        {
            float3 prev = g_final[swizzledDTid].rgb;
            g_final[swizzledDTid].rgb = prev + Li;
        }
        else
            g_final[swizzledDTid].rgb = Li;
    }
}
