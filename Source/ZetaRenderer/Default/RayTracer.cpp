#include "DefaultRendererImpl.h"
#include <Core/CommandList.h>
#include <Core/SharedShaderResources.h>
#include <App/App.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::Util;
using namespace ZetaRay::RT;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

void RayTracer::Init(const RenderSettings& settings, RayTracerData& data)
{
    // Allocate descriptor tables
    data.WndConstDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
        (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::COUNT);
    data.ConstDescTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate(
        (int)RayTracerData::DESC_TABLE_CONST::COUNT);

    // Sun shadow
    data.SunShadowPass.Init();

    Direct3DUtil::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED),
        data.WndConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SUN_SHADOW_DENOISED));

    // Inscattering + sku-view lut
    data.SkyPass.Init(RayTracerData::SKY_LUT_WIDTH, RayTracerData::SKY_LUT_HEIGHT, settings.Inscattering);

    Direct3DUtil::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
        data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::ENV_MAP_SRV));

    if (settings.Inscattering)
    {
        Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
            data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
    }

    {
        data.PreLightingPass.Init();

        const Texture& curvature = data.PreLightingPass.GetCurvatureTexture();
        Direct3DUtil::CreateTexture2DSRV(curvature, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::CURVATURE));
    }

    if (settings.SkyIllumination)
    {
        data.SkyDI_Pass.Init();

        const Texture& denoised = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(denoised, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
    }

    {
        data.IndirecLightingPass.Init(settings.Indirect);

        const Texture& denoised = data.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(denoised, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::INDIRECT_DENOISED));
    }
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data)
{
    // GPU is flushed after resize, safe to reuse descriptors

    if (data.SunShadowPass.IsInitialized())
    {
        data.SunShadowPass.OnWindowResized();

        Direct3DUtil::CreateTexture2DSRV(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED),
            data.WndConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SUN_SHADOW_DENOISED));
    }

    {
        data.PreLightingPass.OnWindowResized();

        const Texture& curvature = data.PreLightingPass.GetCurvatureTexture();
        Direct3DUtil::CreateTexture2DSRV(curvature, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::CURVATURE));
    }

    if (App::GetScene().NumEmissiveInstances())
    {
        data.DirecLightingPass.OnWindowResized();

        const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::DIRECT_LIGHITNG_DENOISED));
    }

    if (data.SkyDI_Pass.IsInitialized())
    {
        data.SkyDI_Pass.OnWindowResized();

        const Texture& t = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
    }

    {
        data.IndirecLightingPass.OnWindowResized();

        const Texture& denoised = data.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(denoised, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::INDIRECT_DENOISED));
    }
}

void RayTracer::Shutdown(RayTracerData& data)
{
    data.ConstDescTable.Reset();
    data.WndConstDescTable.Reset();
    data.RtAS.Clear();
    data.SkyDI_Pass.Reset();
    data.SunShadowPass.Reset();
    data.SkyPass.Reset();
    data.DirecLightingPass.Reset();
    data.PreLightingPass.Reset();
    data.IndirecLightingPass.Reset();
}

void RayTracer::Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, RayTracerData& data)
{
    const auto numEmissives = App::GetScene().NumEmissiveInstances();

    if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
    {
        data.SkyPass.SetInscatteringEnablement(true);

        Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
            data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
    }
    else if (!settings.Inscattering && data.SkyPass.IsInscatteringEnabled())
        data.SkyPass.SetInscatteringEnablement(false);

    if (settings.SkyIllumination && !data.SkyDI_Pass.IsInitialized())
    {
        data.SkyDI_Pass.Init();

        const Texture& skyDIDnsrTex = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(skyDIDnsrTex, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI_DENOISED));
    }
    else if (!settings.SkyIllumination && data.SkyDI_Pass.IsInitialized())
    {
        uint64_t id = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID();
        renderGraph.RemoveResource(id);

        data.SkyDI_Pass.Reset();
    }

    data.RtAS.BuildStaticBLASTransforms();
    data.RtAS.BuildFrameMeshInstanceData();

    data.PreLightingPass.Update();

    // Recompute alias table only if there are stale emissives
    if (numEmissives > 0)
    {
        if (!data.DirecLightingPass.IsInitialized())
        {
            data.DirecLightingPass.Init();

            const Texture& t = data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED);
            Direct3DUtil::CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
                (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::DIRECT_LIGHITNG_DENOISED));

            data.DirecLightingPass.SetLightPresamplingParams(settings.LightPresampling,
                Defaults::NUM_SAMPLE_SETS, Defaults::SAMPLE_SET_SIZE);
        }

        if (App::GetScene().AreEmissivesStale())
        {
            auto& readback = data.PreLightingPass.GetLumenReadbackBuffer();
            data.EmissiveAliasTable.Update(&readback);
            data.EmissiveAliasTable.SetRelaseBuffersDlg(data.PreLightingPass.GetReleaseBuffersDlg());
        }
    }

    data.IndirecLightingPass.SetCurvatureDescHeapOffset(data.WndConstDescTable.GPUDesciptorHeapIndex(
        (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::CURVATURE));
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph)
{
    // Rt AS rebuild/update
    {
        fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.RtAS, &TLAS::Render);
        data.RtASBuildHandle = renderGraph.RegisterRenderPass("RT_AS_Build", RENDER_NODE_TYPE::COMPUTE, dlg1);
    }

    const bool tlasReady = data.RtAS.IsReady();
    const bool hasEmissives = App::GetScene().NumEmissiveInstances() > 0;
    const bool staleEmissives = App::GetScene().AreEmissivesStale();

    // Sky view lut + inscattering
    if (tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SkyPass, &Sky::Render);
        data.SkyHandle = renderGraph.RegisterRenderPass("Sky", RENDER_NODE_TYPE::COMPUTE, dlg);

        auto& skyviewLUT = const_cast<Texture&>(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT));
        renderGraph.RegisterResource(skyviewLUT.Resource(), skyviewLUT.ID(), D3D12_RESOURCE_STATE_COMMON, false);

        if (settings.Inscattering)
        {
            auto& voxelGrid = const_cast<Texture&>(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING));
            renderGraph.RegisterResource(voxelGrid.Resource(), voxelGrid.ID(), D3D12_RESOURCE_STATE_COMMON, false);
        }
    }

    if (tlasReady)
    {
        auto& tlas = const_cast<DefaultHeapBuffer&>(data.RtAS.GetTLAS());
        renderGraph.RegisterResource(tlas.Resource(), tlas.ID(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            false);
    }

    // Sun shadow
    if (tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.SunShadowPass,
            &SunShadow::Render);
        data.SunShadowHandle = renderGraph.RegisterRenderPass("SunShadow", RENDER_NODE_TYPE::COMPUTE, dlg);

        Texture& denoised = const_cast<Texture&>(data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED));
        renderGraph.RegisterResource(denoised.Resource(), denoised.ID());
    }

    // Pre lighting
    fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.PreLightingPass,
        &PreLighting::Render);
    data.PreLightingPassHandle = renderGraph.RegisterRenderPass("PreLighting", RENDER_NODE_TYPE::COMPUTE, dlg1);

    auto& curvatureTex = data.PreLightingPass.GetCurvatureTexture();
    renderGraph.RegisterResource(const_cast<Texture&>(curvatureTex).Resource(), curvatureTex.ID(),
        D3D12_RESOURCE_STATE_COMMON);

    if (hasEmissives)
    {
        // Read back emissive lumen buffer and compute the alias table on CPU
        if (staleEmissives)
        {
            auto& triLumenBuff = data.PreLightingPass.GetLumenBuffer();
            renderGraph.RegisterResource(const_cast<DefaultHeapBuffer&>(triLumenBuff).Resource(), triLumenBuff.ID(), 
                D3D12_RESOURCE_STATE_COPY_SOURCE, false);

            // Make sure to use a separate command list
            fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.EmissiveAliasTable,
                &EmissiveTriangleAliasTable::Render);
            data.EmissiveAliasTableHandle = renderGraph.RegisterRenderPass("EmissiveAliasTable", RENDER_NODE_TYPE::COMPUTE, dlg2, true);

            auto& aliasTable = data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE);
            renderGraph.RegisterResource(aliasTable.Resource(), aliasTable.ID(), D3D12_RESOURCE_STATE_COMMON, false);

            data.EmissiveAliasTable.SetEmissiveTriPassHandle(data.PreLightingPassHandle);
        }

        if (tlasReady)
        {
            // When emissives are stale at frame t:
            // 1. Power for each emissive triangle is calculated (t)
            // 2. Results of step 1 are read back on the CPU and the alias table is built (t)
            // 3. Alias table from step 2 is uploaded to GPU
            // 4. If light presampling is enabled, presampled sets are built each frame starting from frame t + 1
            // 
            // In conclusion, when light presampling is enabled, shaders that depend on it shouldn't execure in frame t.
            if (!settings.LightPresampling || !staleEmissives)
            {
                // Pre lighting
                if (settings.LightPresampling)
                {
                    auto& presampled = data.PreLightingPass.GePresampledSets();
                    renderGraph.RegisterResource(const_cast<DefaultHeapBuffer&>(presampled).Resource(), presampled.ID(),
                        D3D12_RESOURCE_STATE_COMMON);

                    if (settings.UseLVG)
                    {
                        auto& lvg = data.PreLightingPass.GetLightVoxelGrid();
                        renderGraph.RegisterResource(const_cast<DefaultHeapBuffer&>(lvg).Resource(), lvg.ID(),
                            D3D12_RESOURCE_STATE_COMMON);
                    }
                }

                // Direct lighting
                fastdelegate::FastDelegate1<CommandList&> dlg3 = fastdelegate::MakeDelegate(&data.DirecLightingPass,
                    &DirectLighting::Render);
                data.DirecLightingHandle = renderGraph.RegisterRenderPass("DirectLighting", RENDER_NODE_TYPE::COMPUTE, dlg3);

                Texture& td = const_cast<Texture&>(data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED));
                renderGraph.RegisterResource(td.Resource(), td.ID());

                // Indirect lighting
                fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.IndirecLightingPass, &IndirectLighting::Render);
                data.IndirecLightingHandle = renderGraph.RegisterRenderPass("Indirect", RENDER_NODE_TYPE::COMPUTE, dlg2);

                Texture& ti = const_cast<Texture&>(data.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED));
                renderGraph.RegisterResource(ti.Resource(), ti.ID());
            }
        }
    }
    // Indirect lighting
    else if (tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.IndirecLightingPass, &IndirectLighting::Render);
        data.IndirecLightingHandle = renderGraph.RegisterRenderPass("Indirect", RENDER_NODE_TYPE::COMPUTE, dlg2);

        Texture& t = const_cast<Texture&>(data.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED));
        renderGraph.RegisterResource(t.Resource(), t.ID());
    }

    // Sky DI
    if (settings.SkyIllumination && tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.SkyDI_Pass, &SkyDI::Render);
        data.SkyDI_Handle = renderGraph.RegisterRenderPass("SkyDI", RENDER_NODE_TYPE::COMPUTE, dlg2);

        Texture& t = const_cast<Texture&>(data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED));
        renderGraph.RegisterResource(t.Resource(), t.ID());
    }
}

void RayTracer::AddAdjacencies(const RenderSettings& settings, RayTracerData& data, const GBufferData& gbuffData, 
    RenderGraph& renderGraph)
{
    const int outIdx = App::GetRenderer().GlobaIdxForDoubleBufferedResources();
    const bool tlasReady = data.RtAS.IsReady();
    const auto tlasID = tlasReady ? data.RtAS.GetTLAS().ID() : uint64_t(-1);
    const auto numEmissives = App::GetScene().NumEmissiveInstances();

    // Rt AS
    if (tlasReady)
    {
        renderGraph.AddOutput(data.RtASBuildHandle,
            tlasID,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    }

    // Inscattering + sky-view lut
    if (tlasReady)
    {
        // Rt AS
        renderGraph.AddInput(data.SkyHandle,
            tlasID,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        renderGraph.AddOutput(data.SkyHandle,
            data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (settings.Inscattering)
        {
            renderGraph.AddOutput(data.SkyHandle,
                data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).ID(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    renderGraph.AddOutput(data.PreLightingPassHandle,
        data.PreLightingPass.GetCurvatureTexture().ID(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    SmallVector<RenderNodeHandle, Support::SystemAllocator, 3> handles;
    handles.reserve(3);

    handles.push_back(data.IndirecLightingHandle);
    // Direct lighting for emissives runs when:
    // 1. There are emissives
    // 2. Light presampling is disabled, or
    // 3. Emissive aren't stale this frame, which requires alias table to be rebuilt
    if ((numEmissives > 0) && (!settings.LightPresampling || !App::GetScene().AreEmissivesStale()))
        handles.push_back(data.DirecLightingHandle);

    if(settings.SkyIllumination)
        handles.push_back(data.SkyDI_Handle);

    if (numEmissives)
    {
        // Pre lighting
        if (App::GetScene().AreEmissivesStale())
        {
            const auto& triLumenBuff = data.PreLightingPass.GetLumenBuffer();

            renderGraph.AddOutput(data.PreLightingPassHandle,
                triLumenBuff.ID(),
                D3D12_RESOURCE_STATE_COPY_SOURCE);

            renderGraph.AddInput(data.EmissiveAliasTableHandle,
                triLumenBuff.ID(),
                D3D12_RESOURCE_STATE_COPY_SOURCE);

            renderGraph.AddOutput(data.EmissiveAliasTableHandle,
                data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
                D3D12_RESOURCE_STATE_COPY_DEST);
        }

        // Direct + indirect lighting
        if (tlasReady)
        {
            if (App::GetScene().AreEmissivesStale())
            {
                renderGraph.AddInput(data.DirecLightingHandle,
                    data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
                    D3D12_RESOURCE_STATE_COPY_DEST);

                renderGraph.AddInput(data.IndirecLightingHandle,
                    data.EmissiveAliasTable.GetOutput(EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID(),
                    D3D12_RESOURCE_STATE_COPY_DEST);
            }

            if (settings.LightPresampling && !App::GetScene().AreEmissivesStale())
            {
                renderGraph.AddOutput(data.PreLightingPassHandle,
                    data.PreLightingPass.GePresampledSets().ID(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (settings.UseLVG)
                {
                    renderGraph.AddOutput(data.PreLightingPassHandle,
                        data.PreLightingPass.GetLightVoxelGrid().ID(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    renderGraph.AddInput(data.IndirecLightingHandle,
                        data.PreLightingPass.GetLightVoxelGrid().ID(),
                        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                }

                // Presampled sets
                renderGraph.AddInput(data.DirecLightingHandle,
                    data.PreLightingPass.GePresampledSets().ID(),
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

                renderGraph.AddInput(data.IndirecLightingHandle,
                    data.PreLightingPass.GePresampledSets().ID(),
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

                renderGraph.AddOutput(data.DirecLightingHandle,
                    data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
    }

    // Direct + indirect lighting
    if (tlasReady)
    {
        for (int i = 0; i < handles.size(); i++)
        {
            // Rt AS
            renderGraph.AddInput(handles[i],
                tlasID,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

            // Previous g-buffers
            renderGraph.AddInput(handles[i],
                gbuffData.DepthBuffer[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.BaseColor[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.Normal[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.MetallicRoughness[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            // Current g-buffers
            renderGraph.AddInput(handles[i],
                gbuffData.Normal[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.MetallicRoughness[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.DepthBuffer[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.MotionVec.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.BaseColor[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }

        renderGraph.AddInput(data.IndirecLightingHandle,
            data.PreLightingPass.GetCurvatureTexture().ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        // Outputs
        renderGraph.AddOutput(data.IndirecLightingHandle,
            data.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Sun shadow
    if (tlasReady)
    {
        // Rt AS
        renderGraph.AddInput(data.SunShadowHandle,
            tlasID,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        // Make sure it runs post gbuffer
        renderGraph.AddInput(data.SunShadowHandle,
            gbuffData.DepthBuffer[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.SunShadowHandle,
            gbuffData.DepthBuffer[1 - outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.SunShadowHandle,
            gbuffData.Normal[outIdx].ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddInput(data.SunShadowHandle,
            gbuffData.MotionVec.ID(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        renderGraph.AddOutput(data.SunShadowHandle,
            data.SunShadowPass.GetOutput(SunShadow::SHADER_OUT_RES::DENOISED).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Sky DI
    if (settings.SkyIllumination && tlasReady)
    {
        // Denoised output
        renderGraph.AddOutput(data.SkyDI_Handle,
            data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}
