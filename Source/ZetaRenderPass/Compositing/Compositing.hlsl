#include "Compositing_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/LightVoxelGrid.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbCompositing> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);

//--------------------------------------------------------------------------------------
// Helper Functions
//--------------------------------------------------------------------------------------

float3 SunDirectLighting(uint2 DTid, float3 pos, float3 normal, BSDF::ShadingData surface)
{
    Texture2D<float> g_sunShadowTemporalCache = ResourceDescriptorHeap[g_local.SunShadowDescHeapIdx];
    float shadowVal = g_sunShadowTemporalCache[DTid].x;

    // Must match the direction traced in SunShadow pass (RNG seeds must be the same).
    Light::SunSample sunSample = Light::SampleSunDirection(DTid, g_frame.FrameNum, -g_frame.SunDir, 
        g_frame.SunCosAngularRadius, normal, surface);

    // After denoising, specular and non-specular values are averaged together
    // so following needs to be applied again
    sunSample.f *= dot(sunSample.wi, -g_frame.SunDir) >= g_frame.SunCosAngularRadius;

    const float3 sigma_t_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
    const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
    const float3 sigma_t_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;

    pos.y += g_frame.PlanetRadius;

    float t = Volume::IntersectRayAtmosphere(g_frame.PlanetRadius + g_frame.AtmosphereAltitude, 
        pos, -g_frame.SunDir);
    float3 tr = Volume::EstimateTransmittance(g_frame.PlanetRadius, pos, -g_frame.SunDir, t,
        sigma_t_rayleigh, sigma_t_mie, sigma_t_ozone, 8);
    float3 li = g_frame.SunIlluminance * shadowVal * tr * sunSample.f;

    return li;
}

float3 SkyColor(uint2 DTid)
{
    float3 wc = RT::GeneratePinholeCameraRay(DTid, float2(g_frame.RenderWidth, g_frame.RenderHeight), 
        g_frame.AspectRatio, g_frame.TanHalfFOV, g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, 
        g_frame.CurrView[2].xyz, g_frame.CurrCameraJitter);

    float3 rayOrigin = float3(0, 1e-1, 0);
    rayOrigin.y += g_frame.PlanetRadius;

    float3 wTemp = wc;
    // cos(a - b) = cos a cos b + sin a sin b
    wTemp.y = wTemp.y * g_frame.SunCosAngularRadius + sqrt(1 - wc.y * wc.y) * 
        g_frame.SunSinAngularRadius;

    float t;
    bool intersectedPlanet = Volume::IntersectRayPlanet(g_frame.PlanetRadius, rayOrigin, 
        wTemp, t);

    // a disk that's supposed to be the sun
    if (dot(-wc, g_frame.SunDir) >= g_frame.SunCosAngularRadius && !intersectedPlanet)
    {
        return g_frame.SunIlluminance;
    }
    // sample the sky texture
    else
    {
        float2 thetaPhi = Math::SphericalFromCartesian(wc);
        
        const float u = thetaPhi.y * ONE_OVER_2_PI;
        float v = thetaPhi.x * ONE_OVER_PI;
        
        float s = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
        v = (thetaPhi.x - PI_OVER_2) * 0.5f;
        v = 0.5f + s * sqrt(abs(v) * ONE_OVER_PI);

        Texture2D<float3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
        return g_envMap.SampleLevel(g_samLinearClamp, float2(u, v), 0.0f);
    }
}

float3 MapIdxToColor(uint i) 
{
    float r = (i & 0xff) / 255.0f;
    float g = ((i >> 8) & 0xff) / 255.0f;
    float b = ((i >> 16) & 0xff) / 255.0f;

    return float3(r, g, b);
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(COMPOSITING_THREAD_GROUP_DIM_X, COMPOSITING_THREAD_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint Gidx : SV_GroupIndex)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        GBUFFER_OFFSET::METALLIC_ROUGHNESS];
    const float2 mr = g_metallicRoughness[DTid.xy];
    GBuffer::Flags flags = GBuffer::DecodeMetallic(mr.x);

    RWTexture2D<float4> g_composited = ResourceDescriptorHeap[g_local.OutputUAVDescHeapIdx];
    
    if (flags.invalid)
    {
        g_composited[DTid.xy].rgb = SkyColor(DTid.xy);
        return;
    }

    float3 color = 0.0;

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::SUN_DI) && g_local.EmissiveDIDescHeapIdx == 0)
    {
        GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        const float z_view = g_depth[DTid.xy];

        float2 lensSample = 0;
        float3 origin = g_frame.CameraPos;
        if(g_frame.DoF)
        {
            RNG rngDoF = RNG::Init(RNG::PCG3d(DTid.xyx).zy, g_frame.FrameNum);
            lensSample = Sampling::UniformSampleDiskConcentric(rngDoF.Uniform2D());
            lensSample *= g_frame.LensRadius;
        }

        const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        const float3 pos = Math::WorldPosFromScreenSpace2(DTid.xy, renderDim, z_view, 
            g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrCameraJitter, 
            g_frame.CurrView[0].xyz, g_frame.CurrView[1].xyz, g_frame.CurrView[2].xyz, 
            g_frame.DoF, lensSample, g_frame.FocusDepth, origin);

        GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
            GBUFFER_OFFSET::BASE_COLOR];
        const float4 baseColor = flags.subsurface ? g_baseColor[DTid.xy] :
            float4(g_baseColor[DTid.xy].rgb, 0);

        GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::NORMAL];
        const float3 normal = Math::DecodeUnitVector(g_normal[DTid.xy]);

        float eta_curr = ETA_AIR;
        float eta_next = DEFAULT_ETA_MAT;

        if(flags.transmissive)
        {
            GBUFFER_IOR g_ior = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
                GBUFFER_OFFSET::IOR];

            float ior = g_ior[DTid.xy];
            eta_next = GBuffer::DecodeIOR(ior);
        }

        float coat_weight = 0;
        float3 coat_color = 1.0f;
        float coat_roughness = 0;
        float coat_ior = DEFAULT_ETA_COAT;

        if(flags.coated)
        {
            GBUFFER_COAT g_coat = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
                GBUFFER_OFFSET::COAT];
            uint3 packed = g_coat[DTid.xy].xyz;

            GBuffer::Coat coat = GBuffer::UnpackCoat(packed);
            coat_weight = coat.weight;
            coat_color = coat.color;
            coat_roughness = coat.roughness;
            coat_ior = coat.ior;
        }

        const float3 wo = normalize(origin - pos);
        BSDF::ShadingData surface = BSDF::ShadingData::Init(normal, wo, flags.metallic, mr.y, baseColor.xyz,
            eta_curr, eta_next, flags.transmissive, 0, (half)baseColor.w,
            coat_weight, coat_color, coat_roughness, coat_ior);

        color += SunDirectLighting(DTid.xy, pos, normal, surface);
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::SKY_DI) && g_local.SkyDIDescHeapIdx != 0)
    {
        Texture2D<float4> g_sky = ResourceDescriptorHeap[g_local.SkyDIDescHeapIdx];
        float3 ls = g_sky[DTid.xy].rgb;
        color += ls / (g_frame.Accumulate && g_frame.CameraStatic ? g_frame.NumFramesCameraStatic : 1);
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::EMISSIVE_DI) && g_local.EmissiveDIDescHeapIdx != 0)
    {
        Texture2D<float4> g_emissive = ResourceDescriptorHeap[g_local.EmissiveDIDescHeapIdx];
        float3 le = g_emissive[DTid.xy].rgb;
        color += le / (g_frame.Accumulate && g_frame.CameraStatic ? g_frame.NumFramesCameraStatic : 1);
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::INDIRECT) && !flags.emissive && g_local.IndirectDescHeapIdx != 0)
    {
        Texture2D<float4> g_indirect = ResourceDescriptorHeap[g_local.IndirectDescHeapIdx];
        float3 li = g_indirect[DTid.xy].rgb;
        color += li / (g_frame.Accumulate && g_frame.CameraStatic ? g_frame.NumFramesCameraStatic : 1);
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::INSCATTERING))
    {
        GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
            GBUFFER_OFFSET::DEPTH];
        const float z_view = g_depth[DTid.xy];

        if (z_view > 1e-4f)
        {
            float2 posTS = (DTid.xy + 0.5f) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
            float p = pow(max(z_view - g_local.VoxelGridNearZ, 0.0f) / 
                (g_local.VoxelGridFarZ - g_local.VoxelGridNearZ), 1.0f / g_local.DepthMappingExp);
        
            //float p = linearDepth / g_local.VoxelGridDepth;
            //p /= exp(-1.0 + p);
        
            Texture3D<half4> g_voxelGrid = ResourceDescriptorHeap[g_local.InscatteringDescHeapIdx];
            float3 posCube = float3(posTS, p);
            //float3 uvw = float3(posCube.xy, p);
            //const float3 uvw = float3(posTS, 12 / 32.0);
        
            half3 inscattering = g_voxelGrid.SampleLevel(g_samLinearClamp, posCube, 0.0f).rgb;
            color += inscattering;
        }
    }

    if (IS_CB_FLAG_SET(CB_COMPOSIT_FLAGS::VISUALIZE_LVG))
    {
        // int3 voxelIdx;
        // color = 0;

        // GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + 
        //     GBUFFER_OFFSET::DEPTH];
        // const float z_view = g_depth[DTid.xy];
        // float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
        // const float3 pos = Math::WorldPosFromScreenSpace(DTid.xy, renderDim,
        //     z_view, g_frame.TanHalfFOV, g_frame.AspectRatio, g_frame.CurrViewInv, 
        //     g_frame.CurrCameraJitter);

        // if(LVG::MapPosToVoxel(pos, uint3(g_local.GridDim_x, g_local.GridDim_y, g_local.GridDim_z), 
        //     float3(g_local.Extents_x, g_local.Extents_y, g_local.Extents_z), g_frame.CurrView, 
        //     voxelIdx, g_local.Offset_y))
        // {
        //     GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
        //         GBUFFER_OFFSET::BASE_COLOR];
        //     float3 baseColor = g_baseColor[DTid.xy].rgb;

        //     uint3 c = RNG::PCG3d(voxelIdx);
        //     color = baseColor * 0.1 + MapIdxToColor(c.x) * 0.05;
        // }
    }

    g_composited[DTid.xy].rgb = color;
}