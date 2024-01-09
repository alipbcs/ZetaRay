#pragma once

#include <Scene/SceneCore.h>
#include <Core/RendererCore.h>
#include <Core/RenderGraph.h>
#include <Common/FrameConstants.h>
#include <GBuffer/GBufferRT.h>
#include <SunShadow/SunShadow.h>
#include <Compositing/Compositing.h>
#include <TAA/TAA.h>
#include <AutoExposure/AutoExposure.h>
#include <Display/Display.h>
#include <GUI/GuiPass.h>
#include <Sky/Sky.h>
#include <RayTracing/RtAccelerationStructure.h>
//#include <RayTracing/Sampler.h>
#include <FSR2/FSR2.h>
#include <DirectLighting/Emissive/DirectLighting.h>
#include <DirectLighting/Sky/SkyDI.h>
#include <PreLighting/PreLighting.h>
#include <IndirectLighting/IndirectLighting.h>

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer
{
    enum class AA
    {
        NONE,
        TAA,
        FSR2,
        COUNT
    };

    inline static const char* AAOptions[] = { "None", "TAA", "AMD FSR 2.2 (Quality)" };
    static_assert((int)AA::COUNT == ZetaArrayLen(AAOptions), "enum <-> string mismatch.");

    static constexpr auto DEFAULT_AA = AA::TAA;

    struct alignas(64) RenderSettings
    {
        bool Inscattering = false;
        bool SkyIllumination = false;
        bool EmissiveLighting = true;
        AA AntiAliasing = DEFAULT_AA;
    };

    struct alignas(64) GBufferData
    {
        enum GBUFFER
        {
            BASE_COLOR,
            NORMAL,
            METALLIC_ROUGHNESS,
            MOTION_VECTOR,
            EMISSIVE_COLOR,
            TRANSMISSION,
            DEPTH,
            COUNT
        };

        inline static const DXGI_FORMAT GBUFFER_FORMAT[GBUFFER::COUNT] =
        {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R16G16_SNORM,
            DXGI_FORMAT_R8G8_UNORM,
            DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R11G11B10_FLOAT,
            DXGI_FORMAT_R8G8_UNORM,
            DXGI_FORMAT_R32_FLOAT
        };

        // Previous frame's g-buffers are required for denoising and ReSTIR
        Core::GpuMemory::Texture BaseColor[2];
        Core::GpuMemory::Texture Normal[2];
        Core::GpuMemory::Texture MetallicRoughness[2];
        Core::GpuMemory::Texture MotionVec;
        Core::GpuMemory::Texture EmissiveColor;
        Core::GpuMemory::Texture Transmission[2];
        Core::GpuMemory::Texture DepthBuffer[2];

        Core::DescriptorTable SrvDescTable[2];
        Core::DescriptorTable UavDescTable[2];

        RenderPass::GBufferRT GBufferPass;
        Core::RenderNodeHandle GBufferPassHandle;
    };

    struct alignas(64) PostProcessData
    {
        // Render Passes
        RenderPass::Compositing CompositingPass;
        Core::RenderNodeHandle CompositingHandle;

        RenderPass::TAA TaaPass;
        Core::RenderNodeHandle TaaHandle;
        RenderPass::FSR2Pass Fsr2Pass;
        Core::RenderNodeHandle Fsr2Handle;

        RenderPass::AutoExposure AutoExposurePass;
        Core::RenderNodeHandle AutoExposureHandle;

        RenderPass::DisplayPass DisplayPass;
        Core::RenderNodeHandle DisplayHandle;

        RenderPass::GuiPass GuiPass;
        Core::RenderNodeHandle GuiHandle;

        // Descriptors
        enum class DESC_TABLE_CONST
        {
            HDR_LIGHT_ACCUM_SRV,
            EXPOSURE_SRV,
            COUNT
        };

        Core::DescriptorTable WindowSizeConstSRVs;
        Core::DescriptorTable TaaOrFsr2OutSRV;
    };

    struct alignas(64) RayTracerData
    {
        static constexpr int SKY_LUT_WIDTH = 256;
        static constexpr int SKY_LUT_HEIGHT = 128;

        // Scene BVH
        RT::TLAS RtAS;

        // Sampler
        //RT::Sampler RtSampler;

        // Render Passes
        Core::RenderNodeHandle RtASBuildHandle;

        RenderPass::SunShadow SunShadowPass;
        Core::RenderNodeHandle SunShadowHandle;

        RenderPass::SkyDI SkyDI_Pass;
        Core::RenderNodeHandle SkyDI_Handle;

        RenderPass::Sky SkyPass;
        Core::RenderNodeHandle SkyHandle;

        RenderPass::PreLighting PreLightingPass;
        Core::RenderNodeHandle PreLightingPassHandle;

        RenderPass::EmissiveTriangleAliasTable EmissiveAliasTable;
        Core::RenderNodeHandle EmissiveAliasTableHandle;

        RenderPass::DirectLighting DirecLightingPass;
        Core::RenderNodeHandle DirecLightingHandle;

        RenderPass::IndirectLighting IndirecLightingPass;
        Core::RenderNodeHandle IndirecLightingHandle;

        // descriptor tables
        enum class DESC_TABLE_WND_SIZE_CONST
        {
            CURVATURE,
            SKY_DI_DENOISED,
            DIRECT_LIGHITNG_DENOISED,
            SUN_SHADOW_DENOISED,
            INDIRECT_DENOISED,
            COUNT
        };

        enum class DESC_TABLE_CONST
        {
            ENV_MAP_SRV,
            INSCATTERING_SRV,
            COUNT
        };

        Core::DescriptorTable ConstDescTable;
        Core::DescriptorTable WndConstDescTable;
    };

    struct PrivateData
    {
        Core::RenderGraph m_renderGraph;
        Core::GpuMemory::DefaultHeapBuffer m_frameConstantsBuff;

        cbFrameConstants m_frameConstants;
        RenderSettings m_settings;

        GBufferData m_gbuffData;
        PostProcessData m_postProcessorData;
        RayTracerData m_raytracerData;

        AA PendingAA = DEFAULT_AA;
    };

    struct Defaults
    {
        // Ref: S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," Computer Graphics Forum, 2020.
        inline static constexpr Math::float3 SIGMA_S_RAYLEIGH = Math::float3(5.802f, 13.558f, 33.1f) * 1e-3f;    // 1 / km
        inline static constexpr float SIGMA_S_MIE = 3.996f * 1e-3f;        // Mie scattering is not wavelength dependent
        inline static constexpr float SIGMA_A_MIE = 4.4f * 1e-3f;
        inline static constexpr Math::float3 SIGMA_A_OZONE = Math::float3(0.65f, 1.881f, 0.085f) * 1e-3f;
        static constexpr float g = 0.8f;
        static constexpr float ATMOSPHERE_ALTITUDE = 100.0f;        // km
        static constexpr float PLANET_RADIUS = 6360.0f;             // km
    };
}

using Data = ZetaRay::DefaultRenderer::PrivateData;

//--------------------------------------------------------------------------------------
// Common
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::Common
{
    void UpdateFrameConstants(cbFrameConstants& frameConsts, Core::GpuMemory::DefaultHeapBuffer& frameConstsBuff,
        const GBufferData& gbuffData, const RayTracerData& rtData);
}

//--------------------------------------------------------------------------------------
// GBuffer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::GBuffer
{
    void Init(const RenderSettings& settings, GBufferData& data);
    void CreateGBuffers(GBufferData& data);
    void OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data);
    void Shutdown(GBufferData& data);

    void Update(GBufferData& gbuffData);
    void Register(GBufferData& data, const RayTracerData& rayTracerData, Core::RenderGraph& renderGraph);
    void AddAdjacencies(GBufferData& data, const RayTracerData& rayTracerData, 
        Core::RenderGraph& renderGraph);
}

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::RayTracer
{
    void Init(const RenderSettings& settings, RayTracerData& data);
    void OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data);
    void Shutdown(RayTracerData& data);

    void Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, RayTracerData& data);
    void Register(const RenderSettings& settings, RayTracerData& data, Core::RenderGraph& renderGraph);
    void AddAdjacencies(const RenderSettings& settings, RayTracerData& rtData, const GBufferData& gbuffData,
        Core::RenderGraph& renderGraph);
}

//--------------------------------------------------------------------------------------
// PostProcessor
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::PostProcessor
{
    void Init(const RenderSettings& settings, PostProcessData& data);
    void OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data,
        const RayTracerData& rtData);
    void Shutdown(PostProcessData& data);

    void UpdateWndDependentDescriptors(const RenderSettings& settings, PostProcessData& data);
    void UpdateFrameDescriptors(const RenderSettings& settings, PostProcessData& data);
    void UpdatePasses(const RenderSettings& settings, PostProcessData& data);
    void Update(const RenderSettings& settings, PostProcessData& data, const GBufferData& gbuffData,
        const RayTracerData& rayTracerData);
    void Register(const RenderSettings& settings, PostProcessData& data, Core::RenderGraph& renderGraph);
    void AddAdjacencies(const RenderSettings& settings, PostProcessData& data, const GBufferData& gbuffData,
        const RayTracerData& rayTracerData, Core::RenderGraph& renderGraph);
}