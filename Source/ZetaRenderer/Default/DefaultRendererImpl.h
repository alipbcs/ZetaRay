#pragma once

#include <Scene/SceneCore.h>
#include <Core/RenderGraph.h>
#include <Common/FrameConstants.h>
#include <GBuffer/GBufferRT.h>
#include <Compositing/Compositing.h>
#include <TAA/TAA.h>
#include <AutoExposure/AutoExposure.h>
#include <Display/Display.h>
#include <GUI/GuiPass.h>
#include <Sky/Sky.h>
#include <RayTracing/RtAccelerationStructure.h>
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
    struct Defaults
    {
        // Ref: S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," Computer Graphics Forum, 2020.
        inline static constexpr Math::float3 SIGMA_S_RAYLEIGH = Math::float3(5.802f, 13.558f, 33.1f) * 1e-3f;    // 1 / km
        inline static constexpr float SIGMA_S_MIE = 3.996f * 1e-3f;        // Mie scattering is not wavelength dependent
        inline static constexpr float SIGMA_A_MIE = 4.4f * 1e-3f;
        inline static constexpr Math::float3 SIGMA_A_OZONE = Math::float3(0.65f, 1.881f, 0.085f) * 1e-3f;
        static constexpr float g = 0.8f;
        static constexpr float ATMOSPHERE_ALTITUDE = 100.0f;       // km
        static constexpr float PLANET_RADIUS = 6360.0f;            // km
        static constexpr float SUN_ANGULAR_DIAMETER = 0.526f;      // degrees
        static constexpr int NUM_SAMPLE_SETS = 128;
        static constexpr int SAMPLE_SET_SIZE = 512;
        static constexpr float EMISSIVE_SET_MEM_BUDGET_MB = 0.5;
        static constexpr int MIN_NUM_LIGHTS_PRESAMPLING = int((EMISSIVE_SET_MEM_BUDGET_MB * 1024 * 1024) /
            sizeof(RT::PresampledEmissiveTriangle));
        static constexpr Math::uint3 VOXEL_GRID_DIM = Math::uint3(32, 8, 40);
        static constexpr Math::float3 VOXEL_EXTENTS = Math::float3(0.6, 0.45, 0.6);
    };

    enum class AA
    {
        NONE,
        TAA,
        FSR2,
        COUNT
    };

    inline static const char* AAOptions[] = { "None", "TAA", "AMD FSR 2.2 (Quality)" };
    static_assert((int)AA::COUNT == ZetaArrayLen(AAOptions), "enum <-> string mismatch.");

    inline static const char* IndirectOptions[] = { "Path Tracing", "ReSTIR GI", "ReSTIR PT" };
    static_assert((int)RenderPass::IndirectLighting::INTEGRATOR::COUNT == ZetaArrayLen(IndirectOptions), "enum <-> string mismatch.");

    inline static const char* LensTypes[] = { "Pinhole", "Thin Lens" };

    static constexpr auto DEFAULT_AA = AA::TAA;

    struct alignas(64) RenderSettings
    {
        bool Inscattering = false;
        AA AntiAliasing = DEFAULT_AA;
        RenderPass::IndirectLighting::INTEGRATOR Indirect = RenderPass::IndirectLighting::INTEGRATOR::ReSTIR_GI;

        // Presampled sets
        bool LightPresampling = false;

        // LVG
        bool UseLVG = false;
        Math::uint3 VoxelGridDim = Defaults::VOXEL_GRID_DIM;
        Math::float3 VoxelExtents = Defaults::VOXEL_EXTENTS;
        float VoxelGridyOffset = 0.1f;
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
            IOR,
            COAT,
            DEPTH,
            TRI_DIFF_GEO_A,
            TRI_DIFF_GEO_B,
            COUNT
        };

        inline static const DXGI_FORMAT GBUFFER_FORMAT[GBUFFER::COUNT] =
        {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R16G16_UNORM,
            DXGI_FORMAT_R8G8_UNORM,
            DXGI_FORMAT_R16G16_SNORM,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R8_UNORM,
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_R32G32B32A32_UINT,
            DXGI_FORMAT_R32G32_UINT
        };

        // Previous frame's g-buffers are required for denoising and ReSTIR
        Core::GpuMemory::Texture BaseColor[2];
        Core::GpuMemory::Texture Normal[2];
        Core::GpuMemory::Texture MetallicRoughness[2];
        Core::GpuMemory::Texture MotionVec;
        Core::GpuMemory::Texture EmissiveColor;
        Core::GpuMemory::Texture IORBuffer[2];
        Core::GpuMemory::Texture CoatBuffer[2];
        Core::GpuMemory::Texture Depth[2];
        Core::GpuMemory::Texture TriDiffGeo_A[2];
        Core::GpuMemory::Texture TriDiffGeo_B[2];
        Core::GpuMemory::ResourceHeap ResHeap;

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

    struct alignas(64) PathTracerData
    {
        static constexpr int SKY_LUT_WIDTH = 256;
        static constexpr int SKY_LUT_HEIGHT = 128;

        // Scene BVH
        RT::TLAS RtAS;

        // Render Passes
        Core::RenderNodeHandle RtASBuildHandle;

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

        // Reflectance look up texture
        Core::GpuMemory::Texture m_rhoLUT;

        // Descrtiptors
        enum class DESC_TABLE_WND_SIZE_CONST
        {
            SKY_DI,
            EMISSIVE_DI,
            INDIRECT,
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
        Core::GpuMemory::Buffer m_frameConstantsBuff;

        cbFrameConstants m_frameConstants;
        RenderSettings m_settings;

        GBufferData m_gbuffData;
        PostProcessData m_postProcessorData;
        PathTracerData m_pathTracerData;

        AA PendingAA = DEFAULT_AA;
        bool m_sunMoved = false;
        bool m_sceneChanged = false;
    };
}

using Data = ZetaRay::DefaultRenderer::PrivateData;

//--------------------------------------------------------------------------------------
// Common
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::Common
{
    void UpdateFrameConstants(cbFrameConstants& frameConsts, Core::GpuMemory::Buffer& frameConstsBuff,
        const GBufferData& gbuffData, const PathTracerData& rtData);
}

//--------------------------------------------------------------------------------------
// GBuffer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::GBuffer
{
    void Init(const RenderSettings& settings, GBufferData& data);
    void CreateGBuffers(GBufferData& data);
    void OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data);

    void Update(GBufferData& gbuffData);
    void Register(GBufferData& data, const PathTracerData& rayTracerData, Core::RenderGraph& renderGraph);
    void AddAdjacencies(GBufferData& data, const PathTracerData& pathTracerData,
        Core::RenderGraph& renderGraph);
}

//--------------------------------------------------------------------------------------
// PathTracer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::PathTracer
{
    void Init(const RenderSettings& settings, PathTracerData& data);
    void OnWindowSizeChanged(const RenderSettings& settings, PathTracerData& data);

    void Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, PathTracerData& data);
    void Register(const RenderSettings& settings, PathTracerData& data, Core::RenderGraph& renderGraph);
    void AddAdjacencies(const RenderSettings& settings, PathTracerData& rtData, const GBufferData& gbuffData,
        Core::RenderGraph& renderGraph);
}

//--------------------------------------------------------------------------------------
// PostProcessor
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::PostProcessor
{
    void Init(const RenderSettings& settings, PostProcessData& data);
    void OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data,
        const PathTracerData& pathTracerData);

    void UpdateWndDependentDescriptors(const RenderSettings& settings, PostProcessData& data);
    void UpdateFrameDescriptors(const RenderSettings& settings, PostProcessData& data);
    void UpdatePasses(const RenderSettings& settings, PostProcessData& data);
    void Update(const RenderSettings& settings, PostProcessData& data, const GBufferData& gbufferData,
        const PathTracerData& pathTracerData);
    void Register(const RenderSettings& settings, PostProcessData& data, GBufferData& gbufferData,
        Core::RenderGraph& renderGraph);
    void AddAdjacencies(const RenderSettings& settings, PostProcessData& data, 
        const GBufferData& gbufferData,const PathTracerData& pathTracerData,
        Core::RenderGraph& renderGraph);
}