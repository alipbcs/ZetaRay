#include "DefaultRenderer.h"
#include "DefaultRendererImpl.h"
#include <App/Timer.h>
#include <Core/SharedShaderResources.h>
#include <Support/Task.h>
#include <Support/Param.h>
#include <Math/MatrixFuncs.h>
#include <Scene/Camera.h>
#include "../Assets/Font/IconsFontAwesome6.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::Math;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;
using namespace ZetaRay::RenderPass;

namespace
{
    Data* g_data = nullptr;
}

//--------------------------------------------------------------------------------------
// DefaultRenderer::Common
//--------------------------------------------------------------------------------------

void Common::UpdateFrameConstants(cbFrameConstants& frameConsts, Buffer& frameConstsBuff,
    const GBufferData& gbuffData, const RayTracerData& rtData)
{
    auto& renderer = App::GetRenderer();
    const int currIdx = renderer.GlobalIdxForDoubleBufferedResources();

    frameConsts.FrameNum = (uint32_t)App::GetTimer().GetTotalFrameCount();
    frameConsts.dt = (float)App::GetTimer().GetElapsedTime();
    frameConsts.RenderWidth = renderer.GetRenderWidth();
    frameConsts.RenderHeight = renderer.GetRenderHeight();
    frameConsts.DisplayWidth = renderer.GetDisplayWidth();
    frameConsts.DisplayHeight = renderer.GetDisplayHeight();
    frameConsts.CameraRayUVGradsScale = App::GetUpscalingFactor() != 1.0f ?
        powf(2.0f, -(float)frameConsts.RenderWidth / frameConsts.DisplayWidth) :
        1.0f;
    frameConsts.MipBias = App::GetUpscalingFactor() != 1.0f ?
        log2f((float)frameConsts.RenderWidth / frameConsts.DisplayWidth) - 1.0f :
        0.0f;

    frameConsts.BaseColorMapsDescHeapOffset = App::GetScene().GetBaseColMapsDescHeapOffset();
    frameConsts.NormalMapsDescHeapOffset = App::GetScene().GetNormalMapsDescHeapOffset();
    frameConsts.MetallicRoughnessMapsDescHeapOffset = App::GetScene().GetMetallicRougnessMapsDescHeapOffset();
    frameConsts.EmissiveMapsDescHeapOffset = App::GetScene().GetEmissiveMapsDescHeapOffset();

    // Note: assumes BVH has been built
    //frameConsts.WorldRadius = App::GetScene().GetWorldAABB().Extents.length();

    // camera
    const Camera& cam = App::GetCamera();
    v_float4x4 vCurrV = load4x4(const_cast<float4x4a&>(cam.GetCurrView()));
    v_float4x4 vP = load4x4(const_cast<float4x4a&>(cam.GetProj()));
    v_float4x4 vVP = mul(vCurrV, vP);
    float3 prevCameraPos = frameConsts.CameraPos;

    frameConsts.CameraPos = cam.GetPos();
    frameConsts.CameraNear = cam.GetNearZ();
    frameConsts.AspectRatio = cam.GetAspectRatio();
    frameConsts.PixelSpreadAngle = cam.GetPixelSpreadAngle();
    frameConsts.TanHalfFOV = cam.GetTanHalfFOV();
    frameConsts.FocusDepth = cam.GetFocusDepth();
    // mul by 0.5 to get radius from diameter
    frameConsts.LensRadius = 0.5f * (cam.GetFocalLength() / 1000.0f) / cam.GetFStop();
    frameConsts.PrevView = frameConsts.CurrView;
    frameConsts.CurrView = float3x4(cam.GetCurrView());
    frameConsts.PrevViewInv = frameConsts.CurrViewInv;
    frameConsts.CurrViewInv = float3x4(cam.GetViewInv());
    frameConsts.PrevCameraJitter = frameConsts.CurrCameraJitter;
    frameConsts.CurrCameraJitter = cam.GetCurrJitter();
    frameConsts.PrevViewProj = frameConsts.CurrViewProj;
    frameConsts.CurrViewProj = store(vVP);

    // Frame g-buffer SRV descriptor table
    frameConsts.CurrGBufferDescHeapOffset = gbuffData.SrvDescTable[currIdx].GPUDescriptorHeapIndex();
    frameConsts.PrevGBufferDescHeapOffset = gbuffData.SrvDescTable[1 - currIdx].GPUDescriptorHeapIndex();

    // Sky-view LUT SRV
    frameConsts.EnvMapDescHeapOffset = rtData.ConstDescTable.GPUDescriptorHeapIndex(
        (int)RayTracerData::DESC_TABLE_CONST::ENV_MAP_SRV);

    float3 prevViewDir = float3(frameConsts.PrevViewInv.m[0].z, frameConsts.PrevViewInv.m[1].z, 
        frameConsts.PrevViewInv.m[2].z);
    float3 currViewDir = float3(frameConsts.CurrViewInv.m[0].z, frameConsts.CurrViewInv.m[1].z, 
        frameConsts.CurrViewInv.m[2].z);
    float3 delta_pos = prevCameraPos - frameConsts.CameraPos;
    float3 delta_dir = prevViewDir - currViewDir;
    bool cameraStatic = (delta_pos.dot(delta_pos) < FLT_EPSILON) &&
        (delta_dir.dot(delta_dir) < FLT_EPSILON);
    cameraStatic = cameraStatic && !g_data->m_sunMoved && !g_data->m_sceneChanged;
    frameConsts.NumFramesCameraStatic = cameraStatic && frameConsts.Accumulate ? 
        frameConsts.NumFramesCameraStatic + 1 : 0;
    frameConsts.CameraStatic = cameraStatic;
    frameConsts.SunMoved = g_data->m_sunMoved;
    g_data->m_sunMoved = false;
    g_data->m_sceneChanged = false;

    frameConsts.NumEmissiveTriangles = (uint32_t)App::GetScene().NumEmissiveTriangles();
    frameConsts.OneDivNumEmissiveTriangles = 1.0f / frameConsts.NumEmissiveTriangles;

    if (!frameConstsBuff.IsInitialized())
    {
        constexpr size_t sizeInBytes = AlignUp(sizeof(cbFrameConstants), 
            (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        frameConstsBuff = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::FRAME_CONSTANTS_BUFFER,
            (uint32_t)sizeInBytes,
            false,
            MemoryRegion{.Data = &frameConsts, .SizeInBytes = sizeof(cbFrameConstants) });

        renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(GlobalResource::FRAME_CONSTANTS_BUFFER,
            frameConstsBuff);
    }
    else
    {
        GpuMemory::UploadToDefaultHeapBuffer(frameConstsBuff, (uint32_t)sizeof(cbFrameConstants), 
            MemoryRegion{ .Data = &frameConsts, .SizeInBytes = sizeof(cbFrameConstants) });
    }
}

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer
{
    void SetInscatteringEnablement(const ParamVariant& p)
    {
        g_data->m_settings.Inscattering = p.GetBool();
    }

    void SetAA(const ParamVariant& p)
    {
        const int e = p.GetEnum().m_curr;
        Assert(e < (int)AA::COUNT, "Invalid enum value.");
        const AA u = (AA)e;

        if (u == g_data->m_settings.AntiAliasing)
            return;

        float newUpscaleFactor;
        g_data->PendingAA = u;

        switch (u)
        {
        case AA::FSR2:
            if (!g_data->m_postProcessorData.Fsr2Pass.IsInitialized())
                g_data->m_postProcessorData.Fsr2Pass.Init();

            newUpscaleFactor = 1.5f;
            break;
        case AA::NONE:
        case AA::TAA:
        default:
            newUpscaleFactor = 1.0f;
            break;
        }

        App::SetUpscaleFactor(newUpscaleFactor);
    }

    void SetSunDir(const ParamVariant& p)
    {
        float pitch = p.GetUnitDir().m_pitch;
        float yaw = p.GetUnitDir().m_yaw;
        g_data->m_frameConstants.SunDir = -Math::SphericalToCartesian(pitch, yaw);
        g_data->m_sunMoved = true;
    }

    void SetSunLux(const ParamVariant& p)
    {
        g_data->m_frameConstants.SunIlluminance = p.GetFloat().m_value;
        g_data->m_sunMoved = true;
    }

    void SetSunAngularDiameter(const ParamVariant& p)
    {
        auto r = DegreesToRadians(0.5f * p.GetFloat().m_value);
        g_data->m_frameConstants.SunCosAngularRadius = cosf(r);
        g_data->m_sunMoved = true;
    }

    void SetRayleighSigmaSColor(const ParamVariant& p)
    {
        g_data->m_frameConstants.RayleighSigmaSColor = p.GetFloat3().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetRayleighSigmaSScale(const ParamVariant& p)
    {
        g_data->m_frameConstants.RayleighSigmaSScale = p.GetFloat().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetMieSigmaS(const ParamVariant& p)
    {
        g_data->m_frameConstants.MieSigmaS = p.GetFloat().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetMieSigmaA(const ParamVariant& p)
    {
        g_data->m_frameConstants.MieSigmaA = p.GetFloat().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetOzoneSigmaAColor(const ParamVariant& p)
    {
        g_data->m_frameConstants.OzoneSigmaAColor = p.GetColor().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetOzoneSigmaAScale(const ParamVariant& p)
    {
        g_data->m_frameConstants.OzoneSigmaAScale = p.GetFloat().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetgForPhaseHG(const ParamVariant& p)
    {
        g_data->m_frameConstants.g = p.GetFloat().m_value;
        g_data->m_sceneChanged = true;
    }

    void SetAccumulation(const ParamVariant& p)
    {
        g_data->m_frameConstants.Accumulate = p.GetBool();
    }

    void SetIndirect(const ParamVariant& p)
    {
        const int e = p.GetEnum().m_curr;
        Assert(e < (int)IndirectLighting::INTEGRATOR::COUNT, "Invalid enum value.");
        g_data->m_settings.Indirect = (IndirectLighting::INTEGRATOR)e;
        g_data->m_raytracerData.IndirecLightingPass.SetMethod(g_data->m_settings.Indirect);
    }

    void VoxelExtentsCallback(const ParamVariant& p)
    {
        g_data->m_settings.VoxelExtents = p.GetFloat3().m_value;

        g_data->m_raytracerData.PreLightingPass.SetLightVoxelGridParams(true, g_data->m_settings.VoxelGridDim,
            g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
        g_data->m_raytracerData.IndirecLightingPass.SetLightVoxelGridParams(true, g_data->m_settings.VoxelGridDim,
            g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
        g_data->m_postProcessorData.CompositingPass.SetLightVoxelGridParams(g_data->m_settings.VoxelGridDim,
            g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
    }

    void YOffsetCallback(const ParamVariant& p)
    {
        g_data->m_settings.VoxelGridyOffset = p.GetFloat().m_value;

        g_data->m_raytracerData.PreLightingPass.SetLightVoxelGridParams(true, g_data->m_settings.VoxelGridDim,
            g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
        g_data->m_raytracerData.IndirecLightingPass.SetLightVoxelGridParams(true, g_data->m_settings.VoxelGridDim,
            g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
        g_data->m_postProcessorData.CompositingPass.SetLightVoxelGridParams(g_data->m_settings.VoxelGridDim,
            g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
    }

    void SetLVGEnablement(bool enable)
    {
        if (enable)
        {
            ParamVariant extents;
            extents.InitFloat3("Renderer", "Light Voxel Grid", "Extents",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::VoxelExtentsCallback),
                g_data->m_settings.VoxelExtents,
                0.1,
                2,
                0.1);
            App::AddParam(extents);

            ParamVariant offset_y;
            offset_y.InitFloat("Renderer", "Light Voxel Grid", "Y Offset",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::YOffsetCallback),
                g_data->m_settings.VoxelGridyOffset,
                0,
                2,
                0.1);
            App::AddParam(offset_y);

            g_data->m_raytracerData.PreLightingPass.SetLightVoxelGridParams(enable, g_data->m_settings.VoxelGridDim,
                g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
            g_data->m_raytracerData.IndirecLightingPass.SetLightVoxelGridParams(enable, g_data->m_settings.VoxelGridDim,
                g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
            g_data->m_postProcessorData.CompositingPass.SetLightVoxelGridParams(g_data->m_settings.VoxelGridDim,
                g_data->m_settings.VoxelExtents, g_data->m_settings.VoxelGridyOffset);
        }
        else
        {
            App::RemoveParam("Renderer", "Light Voxel Grid", "Extents");
            App::RemoveParam("Renderer", "Light Voxel Grid", "Y Offset");

            g_data->m_raytracerData.PreLightingPass.SetLightVoxelGridParams(false, uint3(0), float3(0), 0);
            g_data->m_raytracerData.IndirecLightingPass.SetLightVoxelGridParams(false, uint3(0), float3(0), 0);
        }
    }

    void SetLVG(const ParamVariant& p)
    {
        bool newVal = p.GetBool();
        if (newVal == g_data->m_settings.UseLVG)
            return;

        g_data->m_settings.UseLVG = newVal;
        SetLVGEnablement(newVal);
    }

    void SetLensType(const Support::ParamVariant& p)
    {
        g_data->m_frameConstants.DoF = p.GetEnum().m_curr;
        g_data->m_sceneChanged = true;
    }
}

namespace ZetaRay::DefaultRenderer
{
    void Init()
    {
        Assert(g_data->PendingAA == g_data->m_settings.AntiAliasing, "These must match.");
        memset(&g_data->m_frameConstants, 0, sizeof(cbFrameConstants));

        g_data->m_renderGraph.Reset();

        const Camera& cam = App::GetCamera();
        v_float4x4 vCurrV = load4x4(const_cast<float4x4a&>(cam.GetCurrView()));

        // For 1st frame
        v_float4x4 vP = load4x4(const_cast<float4x4a&>(cam.GetProj()));
        v_float4x4 vVP = mul(vCurrV, vP);
        g_data->m_frameConstants.PrevViewInv = float3x4(cam.GetViewInv());
        g_data->m_frameConstants.PrevView = float3x4(cam.GetCurrView());
        g_data->m_frameConstants.CurrViewProj = store(vVP);

        //g_data->m_frameConstants.SunDir = float3(0.223f, -0.96f, -0.167f);
        g_data->m_frameConstants.SunDir = float3(0.6565358f, -0.0560669f, 0.752208233f);
        //g_data->m_frameConstants.SunDir = float3(0.0f, 1.0f, 0.0f);
        g_data->m_frameConstants.SunDir.normalize();
        g_data->m_frameConstants.SunIlluminance = 20.0f;
        constexpr float angularRadius = DegreesToRadians(0.5f * Defaults::SUN_ANGULAR_DIAMETER);
        g_data->m_frameConstants.SunCosAngularRadius = cosf(angularRadius);
        g_data->m_frameConstants.SunSinAngularRadius = sqrtf(1.0f - g_data->m_frameConstants.SunCosAngularRadius * 
            g_data->m_frameConstants.SunCosAngularRadius);
        g_data->m_frameConstants.AtmosphereAltitude = Defaults::ATMOSPHERE_ALTITUDE;
        g_data->m_frameConstants.PlanetRadius = Defaults::PLANET_RADIUS;
        g_data->m_frameConstants.g = Defaults::g;
        g_data->m_frameConstants.NumFramesCameraStatic = 0;
        g_data->m_frameConstants.Accumulate = false;
        g_data->m_frameConstants.DoF = 0;

        auto normalizeAndStore = [](float3 v, float3& cbVal, float& cbScale)
        {
            float scale = v.length();
            cbScale = scale;

            if (scale >= FLT_EPSILON)
            {
                float scaleRcp = 1.0f / scale;
                cbVal = v * scaleRcp;
            }
        };

        normalizeAndStore(Defaults::SIGMA_S_RAYLEIGH, g_data->m_frameConstants.RayleighSigmaSColor, 
            g_data->m_frameConstants.RayleighSigmaSScale);
        normalizeAndStore(float3(Defaults::SIGMA_A_OZONE), g_data->m_frameConstants.OzoneSigmaAColor, 
            g_data->m_frameConstants.OzoneSigmaAScale);

        g_data->m_frameConstants.MieSigmaA = Defaults::SIGMA_A_MIE;
        g_data->m_frameConstants.MieSigmaS = Defaults::SIGMA_S_MIE;

        TaskSet ts;
        ts.EmplaceTask("GBuffer_Init", []()
            {
                GBuffer::Init(g_data->m_settings, g_data->m_gbuffData);
            });
        ts.EmplaceTask("RayTracer_Init", []()
            {
                RayTracer::Init(g_data->m_settings, g_data->m_raytracerData);
            });
        ts.EmplaceTask("PostProcessor_Init", []()
            {
                PostProcessor::Init(g_data->m_settings, g_data->m_postProcessorData);
            });

        ts.Sort();
        ts.Finalize();
        App::Submit(ZetaMove(ts));

        // Render settings
        {
            ParamVariant enableInscattering;
            enableInscattering.InitBool(ICON_FA_FILM " Renderer", "Compositing", "Inscattering",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetInscatteringEnablement),
                g_data->m_settings.Inscattering);
            App::AddParam(enableInscattering);

            ParamVariant p;
            p.InitEnum(ICON_FA_FILM " Renderer", "Anti-Aliasing", "Method",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetAA),
                AAOptions, ZetaArrayLen(AAOptions), (int)g_data->m_settings.AntiAliasing);
            App::AddParam(p);

            ParamVariant p1;
            p1.InitBool(ICON_FA_FILM " Renderer", "Compositing", "Accumulate",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetAccumulation),
                g_data->m_frameConstants.Accumulate);
            App::AddParam(p1);

            ParamVariant p2;
            p2.InitEnum(ICON_FA_FILM " Renderer", "Indirect Lighting", "Integrator",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetIndirect),
                IndirectOptions, ZetaArrayLen(IndirectOptions), (int)g_data->m_settings.Indirect);
            App::AddParam(p2);

            ParamVariant p3;
            p3.InitEnum(ICON_FA_LANDMARK " Scene", "Camera", "Type",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetLensType),
                LensTypes, ZetaArrayLen(LensTypes), 0, "Lens");
            App::AddParam(p3);

            g_data->m_settings.LightPresampling = App::GetScene().NumEmissiveTriangles() >= Defaults::MIN_NUM_LIGHTS_PRESAMPLING;
            g_data->m_settings.UseLVG = g_data->m_settings.UseLVG && g_data->m_settings.LightPresampling;
        }

        // Sun
        {
            ParamVariant p0;
            p0.InitUnitDir(ICON_FA_LANDMARK " Scene", "Sun", "(-)Dir",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetSunDir),
                -g_data->m_frameConstants.SunDir);
            App::AddParam(p0);

            ParamVariant p1;
            p1.InitFloat(ICON_FA_LANDMARK " Scene", "Sun", "Illuminance",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetSunLux),
                g_data->m_frameConstants.SunIlluminance, 1.0f, 100.0f, 1.0f);
            App::AddParam(p1);

            ParamVariant p2;
            p2.InitFloat(ICON_FA_LANDMARK " Scene", "Sun", "Angular Diameter (degrees)",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetSunAngularDiameter),
                Defaults::SUN_ANGULAR_DIAMETER, 0.1f, 10.0f, 1e-2f);
            App::AddParam(p2);
        }

        // Atmosphere
        {
            ParamVariant p0;
            p0.InitColor(ICON_FA_LANDMARK " Scene", "Atmosphere", "Rayleigh scattering color",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetRayleighSigmaSColor),
                g_data->m_frameConstants.RayleighSigmaSColor);
            App::AddParam(p0);

            ParamVariant p1;
            p1.InitFloat(ICON_FA_LANDMARK " Scene", "Atmosphere", "Rayleigh scattering scale",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetRayleighSigmaSScale),
                g_data->m_frameConstants.RayleighSigmaSScale, 0.0f, 10.0f, 1e-3f);
            App::AddParam(p1);

            ParamVariant p2;
            p2.InitFloat(ICON_FA_LANDMARK " Scene", "Atmosphere", "Mie scattering coeff.",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetMieSigmaS),
                Defaults::SIGMA_S_MIE, 1e-6f, 1e-1f, 1e-3f);
            App::AddParam(p2);

            ParamVariant p3;
            p3.InitFloat(ICON_FA_LANDMARK " Scene", "Atmosphere", "Mie absorption coeff.",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetMieSigmaA),
                Defaults::SIGMA_A_MIE, 1e-6f, 10.0f, 1e-3f);
            App::AddParam(p3);

            ParamVariant p4;
            p4.InitFloat(ICON_FA_LANDMARK " Scene", "Atmosphere", "Ozone absorption scale",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetOzoneSigmaAScale),
                g_data->m_frameConstants.OzoneSigmaAScale, 0.0f, 10.0f, 1e-4f);
            App::AddParam(p4);

            ParamVariant p5;
            p5.InitColor(ICON_FA_LANDMARK " Scene", "Atmosphere", "Ozone absorption color",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetOzoneSigmaAColor),
                g_data->m_frameConstants.OzoneSigmaAColor);
            App::AddParam(p5);

            ParamVariant p6;
            p6.InitFloat(ICON_FA_LANDMARK " Scene", "Atmosphere", "g (HG Phase Function)",
                fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetgForPhaseHG),
                Defaults::g, -0.99f, 0.99f, 0.2f);
            App::AddParam(p6);
        }
    }

    void Update(TaskSet& ts)
    {
        g_data->m_settings.AntiAliasing = g_data->PendingAA;
        const auto frame = App::GetTimer().GetTotalFrameCount();

        if (frame <= 1 && App::GetScene().NumEmissiveInstances())
        {
            g_data->m_settings.LightPresampling = App::GetScene().NumEmissiveTriangles() >= 
                Defaults::MIN_NUM_LIGHTS_PRESAMPLING;
            //g_data->m_settings.UseLVG = g_data->m_settings.UseLVG && g_data->m_settings.LightPresampling;

            if (g_data->m_settings.LightPresampling)
            {
                // Notes:
                // 1. Light presampling is off by default. So the following calls are only needed when it's been enabled.
                // 2. Render graph ensures alias table and presampled sets are already computed when GPU 
                //    accesses them in the following render passes.
                g_data->m_raytracerData.PreLightingPass.SetLightPresamplingParams(
                    Defaults::MIN_NUM_LIGHTS_PRESAMPLING,
                    Defaults::NUM_SAMPLE_SETS, Defaults::SAMPLE_SET_SIZE);
                g_data->m_raytracerData.IndirecLightingPass.SetLightPresamplingParams(true,
                    Defaults::NUM_SAMPLE_SETS, Defaults::SAMPLE_SET_SIZE);

                //ParamVariant p;
                //p.InitBool("Renderer", "Lighting", "Light Voxel Grid", 
                //    fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetLVG),
                //    g_data->m_settings.UseLVG);
                //App::AddParam(p);
            }
            else
            {
                g_data->m_raytracerData.PreLightingPass.SetLightPresamplingParams(
                    Defaults::MIN_NUM_LIGHTS_PRESAMPLING, 0, 0);

                //App::RemoveParam("Renderer", "Lighting", "Light Voxel Grid");
            }

            //SetLVGEnablement(g_data->m_settings.UseLVG);
        }

        auto h0 = ts.EmplaceTask("SceneRenderer::UpdatePasses", []()
            {
                GBuffer::Update(g_data->m_gbuffData);
                RayTracer::Update(g_data->m_settings, g_data->m_renderGraph, g_data->m_raytracerData);
                PostProcessor::Update(g_data->m_settings, g_data->m_postProcessorData, g_data->m_gbuffData,
                    g_data->m_raytracerData);
                Common::UpdateFrameConstants(g_data->m_frameConstants, g_data->m_frameConstantsBuff, g_data->m_gbuffData, 
                    g_data->m_raytracerData);
            });

        auto h3 = ts.EmplaceTask("SceneRenderer::RenderGraph", []()
            {
                g_data->m_renderGraph.BeginFrame();

                GBuffer::Register(g_data->m_gbuffData, g_data->m_raytracerData, g_data->m_renderGraph);
                RayTracer::Register(g_data->m_settings, g_data->m_raytracerData, g_data->m_renderGraph);
                PostProcessor::Register(g_data->m_settings, g_data->m_postProcessorData, g_data->m_gbuffData,
                    g_data->m_renderGraph);

                g_data->m_renderGraph.MoveToPostRegister();

                GBuffer::AddAdjacencies(g_data->m_gbuffData, g_data->m_raytracerData, g_data->m_renderGraph);
                RayTracer::AddAdjacencies(g_data->m_settings, g_data->m_raytracerData, g_data->m_gbuffData,
                    g_data->m_renderGraph);
                PostProcessor::AddAdjacencies(g_data->m_settings, g_data->m_postProcessorData, g_data->m_gbuffData,
                    g_data->m_raytracerData, g_data->m_renderGraph);
            });

        // Render graph should go last
        ts.AddOutgoingEdge(h0, h3);
    }

    void Render(TaskSet& ts)
    {
        g_data->m_renderGraph.Build(ts);
    }

    void Shutdown()
    {
        g_data->m_renderGraph.Shutdown();

        // At this point, GPU has been flushed, so extra synchronization is not needed
        delete g_data;
    }

    void OnWindowSizeChanged()
    {
        // Following order is important
        GBuffer::OnWindowSizeChanged(g_data->m_settings, g_data->m_gbuffData);
        RayTracer::OnWindowSizeChanged(g_data->m_settings, g_data->m_raytracerData);
        PostProcessor::OnWindowSizeChanged(g_data->m_settings, g_data->m_postProcessorData, g_data->m_raytracerData);

        g_data->m_renderGraph.Reset();
    }

    Core::RenderGraph* GetRenderGraph()
    {
        return &g_data->m_renderGraph;
    }

    void DebugDrawRenderGraph()
    {
        g_data->m_renderGraph.DebugDrawGraph();
    }

    bool IsRTASBuilt()
    {
        return g_data->m_raytracerData.RtAS.GetTLAS().IsInitialized();
    }

    void SceneModified()
    {
        g_data->m_sceneChanged = true;
    }

    void Pick(uint16 screenPosX, uint16 screenPosY)
    {
        g_data->m_gbuffData.GBufferPass.PickPixel(screenPosX, screenPosY);
    }

    void ClearPick()
    {
        g_data->m_postProcessorData.DisplayPass.ClearPick();
    }

    void CaptureScreen()
    {
        g_data->m_postProcessorData.DisplayPass.CaptureScreen();
    }
}

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

Scene::Renderer::Interface DefaultRenderer::InitAndGetInterface()
{
    Assert(!g_data, "g_data has already been initialized.");
    g_data = new (std::nothrow) Data;

    Scene::Renderer::Interface rndIntrf;

    rndIntrf.Init = &DefaultRenderer::Init;
    rndIntrf.Update = &DefaultRenderer::Update;
    rndIntrf.Render = &DefaultRenderer::Render;
    rndIntrf.Shutdown = &DefaultRenderer::Shutdown;
    rndIntrf.OnWindowSizeChanged = &DefaultRenderer::OnWindowSizeChanged;
    rndIntrf.GetRenderGraph = &DefaultRenderer::GetRenderGraph;
    rndIntrf.DebugDrawRenderGraph = &DefaultRenderer::DebugDrawRenderGraph;
    rndIntrf.IsRTASBuilt = &DefaultRenderer::IsRTASBuilt;
    rndIntrf.SceneModified = &DefaultRenderer::SceneModified;
    rndIntrf.Pick = &DefaultRenderer::Pick;
    rndIntrf.ClearPick = &DefaultRenderer::ClearPick;
    rndIntrf.CaptureScreen = &DefaultRenderer::CaptureScreen;

    return rndIntrf;
}