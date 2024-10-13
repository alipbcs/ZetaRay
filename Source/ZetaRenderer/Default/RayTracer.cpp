#include "DefaultRendererImpl.h"
#include <App/Timer.h>

using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::Util;
using namespace ZetaRay::RT;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Core::Direct3DUtil;
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

    // Inscattering + sku-view lut
    data.SkyPass.Init(RayTracerData::SKY_LUT_WIDTH, RayTracerData::SKY_LUT_HEIGHT, 
        settings.Inscattering);

    Direct3DUtil::CreateTexture2DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT),
        data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::ENV_MAP_SRV));

    if (settings.Inscattering)
    {
        Direct3DUtil::CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
            data.ConstDescTable.CPUHandle((int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
    }

    data.PreLightingPass.Init();

    {
        data.IndirecLightingPass.Init(settings.Indirect);

        const Texture& denoised = data.IndirecLightingPass.GetOutput(
            IndirectLighting::SHADER_OUT_RES::DENOISED);
        Direct3DUtil::CreateTexture2DSRV(denoised, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::INDIRECT));
    }
}

void RayTracer::OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data)
{
    // GPU is flushed after resize, safe to reuse descriptors

    data.PreLightingPass.OnWindowResized();

    if (App::GetScene().NumEmissiveInstances())
    {
        data.DirecLightingPass.OnWindowResized();

        const Texture& t = data.DirecLightingPass.GetOutput(
            DirectLighting::SHADER_OUT_RES::DENOISED);
        CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::EMISSIVE_DI));
    }

    if (data.SkyDI_Pass.IsInitialized())
    {
        data.SkyDI_Pass.OnWindowResized();

        const Texture& t = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
        CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI));
    }

    {
        data.IndirecLightingPass.OnWindowResized();

        const Texture& denoised = data.IndirecLightingPass.GetOutput(
            IndirectLighting::SHADER_OUT_RES::DENOISED);
        CreateTexture2DSRV(denoised, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::INDIRECT));
    }
}

void RayTracer::Update(const RenderSettings& settings, Core::RenderGraph& renderGraph, 
    RayTracerData& data)
{
    const auto numEmissives = App::GetScene().NumEmissiveInstances();

    if (settings.Inscattering && !data.SkyPass.IsInscatteringEnabled())
    {
        data.SkyPass.SetInscatteringEnablement(true);

        CreateTexture3DSRV(data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING),
            data.ConstDescTable.CPUHandle(
                (int)RayTracerData::DESC_TABLE_CONST::INSCATTERING_SRV));
    }
    else if (!settings.Inscattering && data.SkyPass.IsInscatteringEnabled())
        data.SkyPass.SetInscatteringEnablement(false);

    if (numEmissives == 0 && !data.SkyDI_Pass.IsInitialized())
    {
        data.SkyDI_Pass.Init();

        const Texture& skyDIOutTex = data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED);
        CreateTexture2DSRV(skyDIOutTex, data.WndConstDescTable.CPUHandle(
            (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::SKY_DI));
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
            CreateTexture2DSRV(t, data.WndConstDescTable.CPUHandle(
                (int)RayTracerData::DESC_TABLE_WND_SIZE_CONST::EMISSIVE_DI));

            data.DirecLightingPass.SetLightPresamplingParams(settings.LightPresampling,
                Defaults::NUM_SAMPLE_SETS, Defaults::SAMPLE_SET_SIZE);
        }

        if (App::GetScene().AreEmissivesStale())
        {
            auto& readback = data.PreLightingPass.GetLumenReadbackBuffer();
            data.EmissiveAliasTable.Update(&readback);
            data.EmissiveAliasTable.SetReleaseBuffersDlg(data.PreLightingPass.GetReleaseBuffersDlg());
        }
    }
}

void RayTracer::Register(const RenderSettings& settings, RayTracerData& data, 
    RenderGraph& renderGraph)
{
    // Rt AS rebuild/update
    {
        fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(
            &data.RtAS, &TLAS::Render);
        data.RtASBuildHandle = renderGraph.RegisterRenderPass("RT_AS_Build", 
            RENDER_NODE_TYPE::COMPUTE, dlg1);
    }

    const bool tlasReady = data.RtAS.IsReady();
    const bool hasEmissives = App::GetScene().NumEmissiveInstances() > 0;
    const auto frame = App::GetTimer().GetTotalFrameCount();

    // Sky-view lut + inscattering
    if (tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(
            &data.SkyPass, &Sky::Render);
        data.SkyHandle = renderGraph.RegisterRenderPass("Sky", RENDER_NODE_TYPE::COMPUTE, 
            dlg);

        auto& skyviewLUT = const_cast<Texture&>(data.SkyPass.GetOutput(
            Sky::SHADER_OUT_RES::SKY_VIEW_LUT));
        renderGraph.RegisterResource(skyviewLUT.Resource(), skyviewLUT.ID(), 
            D3D12_RESOURCE_STATE_COMMON, false);

        if (settings.Inscattering)
        {
            auto& voxelGrid = const_cast<Texture&>(data.SkyPass.GetOutput(
                Sky::SHADER_OUT_RES::INSCATTERING));
            renderGraph.RegisterResource(voxelGrid.Resource(), voxelGrid.ID(), 
                D3D12_RESOURCE_STATE_COMMON, false);
        }
    }

    if (tlasReady)
    {
        auto& tlas = const_cast<Buffer&>(data.RtAS.GetTLAS());
        renderGraph.RegisterResource(tlas.Resource(), tlas.ID(), 
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            false);
    }

    if (hasEmissives)
    {
        // Pre lighting
        fastdelegate::FastDelegate1<CommandList&> dlg1 = fastdelegate::MakeDelegate(&data.PreLightingPass,
            &PreLighting::Render);
        data.PreLightingPassHandle = renderGraph.RegisterRenderPass("PreLighting", 
            RENDER_NODE_TYPE::COMPUTE, dlg1);

        // Read back emissive lumen buffer and compute alias table on CPU
        if (App::GetScene().AreEmissivesStale())
        {
            auto& triLumenBuff = data.PreLightingPass.GetLumenBuffer();
            renderGraph.RegisterResource(const_cast<Buffer&>(triLumenBuff).Resource(), 
                triLumenBuff.ID(), D3D12_RESOURCE_STATE_COPY_SOURCE, false);

            fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.EmissiveAliasTable,
                &EmissiveTriangleAliasTable::Render);
            data.EmissiveAliasTableHandle = renderGraph.RegisterRenderPass("EmissiveAliasTable", 
                RENDER_NODE_TYPE::COMPUTE, dlg2);

            auto& aliasTable = data.EmissiveAliasTable.GetOutput(
                EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE);
            renderGraph.RegisterResource(aliasTable.Resource(), aliasTable.ID(), 
                D3D12_RESOURCE_STATE_COMMON, false);

            data.EmissiveAliasTable.SetEmissiveTriPassHandle(data.PreLightingPassHandle);
        }
        // Since alias table is computed on CPU, instead of waiting for GPU to finish
        // computation and causing a hitch, defer computation to next frame(s) at
        // the expense of some lag
        else if (data.EmissiveAliasTable.HasPendingRender())
        {
            fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(&data.EmissiveAliasTable,
                &EmissiveTriangleAliasTable::Render);
            data.EmissiveAliasTableHandle = renderGraph.RegisterRenderPass("EmissiveAliasTable",
                RENDER_NODE_TYPE::COMPUTE, dlg2);

            // Refer to notes in lines 413-415
            if (settings.LightPresampling)
            {
                auto& aliasTable = data.EmissiveAliasTable.GetOutput(
                    EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE);
                renderGraph.RegisterResource(aliasTable.Resource(), aliasTable.ID(),
                    D3D12_RESOURCE_STATE_COMMON, false);
            }
        }

        if (tlasReady)
        {
            // At frame 1 (app startup is counted as "frame" 0, so program
            // loop starts from frame 1):
            // 1. Power of each emissive triangle is estimated (1)
            // 2. Results of step 1 are read back on CPU and alias table is built (1)
            // 3. Alias table is uploaded to GPU
            // 4. If light presampling is enabled, presampled sets are built each frame 
            //    using the alias table starting from next frame (2 - one frame of delay)
            // 
            // In conclusion, when light presampling is enabled, shaders that depend on it 
            // shouldn't execute in frame 1.
            const bool presampledSetsBuiltOnce = frame > 1;

            if (!settings.LightPresampling || presampledSetsBuiltOnce)
            {
                // Pre lighting
                if (settings.LightPresampling && presampledSetsBuiltOnce)
                {
                    auto& presampled = data.PreLightingPass.GePresampledSets();
                    renderGraph.RegisterResource(const_cast<Buffer&>(presampled).Resource(), 
                        presampled.ID(), D3D12_RESOURCE_STATE_COMMON);

                    if (settings.UseLVG)
                    {
                        auto& lvg = data.PreLightingPass.GetLightVoxelGrid();
                        renderGraph.RegisterResource(const_cast<Buffer&>(lvg).Resource(), lvg.ID(),
                            D3D12_RESOURCE_STATE_COMMON);
                    }
                }

                // Direct lighting
                fastdelegate::FastDelegate1<CommandList&> dlg3 = fastdelegate::MakeDelegate(&data.DirecLightingPass,
                    &DirectLighting::Render);
                data.DirecLightingHandle = renderGraph.RegisterRenderPass("DirectLighting", 
                    RENDER_NODE_TYPE::COMPUTE, dlg3);

                Texture& td = const_cast<Texture&>(data.DirecLightingPass.GetOutput(
                    DirectLighting::SHADER_OUT_RES::DENOISED));
                renderGraph.RegisterResource(td.Resource(), td.ID());

                // Indirect lighting
                fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(
                    &data.IndirecLightingPass, &IndirectLighting::Render);
                data.IndirecLightingHandle = renderGraph.RegisterRenderPass("Indirect", 
                    RENDER_NODE_TYPE::COMPUTE, dlg2);

                Texture& ti = const_cast<Texture&>(data.IndirecLightingPass.GetOutput(
                    IndirectLighting::SHADER_OUT_RES::DENOISED));
                renderGraph.RegisterResource(ti.Resource(), ti.ID());
            }
        }
    }
    // Indirect lighting
    else if (tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(
            &data.IndirecLightingPass, &IndirectLighting::Render);
        data.IndirecLightingHandle = renderGraph.RegisterRenderPass("Indirect", 
            RENDER_NODE_TYPE::COMPUTE, dlg2);

        Texture& t = const_cast<Texture&>(data.IndirecLightingPass.GetOutput(
            IndirectLighting::SHADER_OUT_RES::DENOISED));
        renderGraph.RegisterResource(t.Resource(), t.ID());
    }

    // Sky DI
    if (!hasEmissives && tlasReady)
    {
        fastdelegate::FastDelegate1<CommandList&> dlg2 = fastdelegate::MakeDelegate(
            &data.SkyDI_Pass, &SkyDI::Render);
        data.SkyDI_Handle = renderGraph.RegisterRenderPass("SkyDI", RENDER_NODE_TYPE::COMPUTE, 
            dlg2);

        Texture& t = const_cast<Texture&>(data.SkyDI_Pass.GetOutput(
            SkyDI::SHADER_OUT_RES::DENOISED));
        renderGraph.RegisterResource(t.Resource(), t.ID());
    }
}

void RayTracer::AddAdjacencies(const RenderSettings& settings, RayTracerData& data, 
    const GBufferData& gbuffData, RenderGraph& renderGraph)
{
    const int outIdx = App::GetRenderer().GlobalIdxForDoubleBufferedResources();
    const bool tlasReady = data.RtAS.IsReady();
    const auto tlasID = tlasReady ? data.RtAS.GetTLAS().ID() : GpuMemory::INVALID_ID;
    const auto numEmissives = App::GetScene().NumEmissiveInstances();
    const auto frame = App::GetTimer().GetTotalFrameCount();

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
        renderGraph.AddOutput(data.SkyHandle,
            data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::SKY_VIEW_LUT).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (settings.Inscattering)
        {
            // Rt AS
            renderGraph.AddInput(data.SkyHandle,
                tlasID,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

            renderGraph.AddOutput(data.SkyHandle,
                data.SkyPass.GetOutput(Sky::SHADER_OUT_RES::INSCATTERING).ID(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    SmallVector<RenderNodeHandle, Support::SystemAllocator, 3> handles;
    handles.reserve(3);
    handles.push_back(data.IndirecLightingHandle);
    
    // When light presampling is enabled, sample sets are available starting frame frame 2
    const bool presampledSetsBuiltOnce = frame > 1;
    if ((numEmissives > 0) && (!settings.LightPresampling || presampledSetsBuiltOnce))
        handles.push_back(data.DirecLightingHandle);

    if(numEmissives == 0)
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
        // Tri lumen buffer was recomputed last frame, but the results weren't ready yet. Now
        // in this frame, alias table pass has no dependencies, but prelighting should run
        // after it so the new alias table is used as early as possible for presampling.
        else if (settings.LightPresampling && data.EmissiveAliasTable.HasPendingRender())
        {
            const uint32_t aliasTable = data.EmissiveAliasTable.GetOutput(
                EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID();

            renderGraph.AddOutput(data.EmissiveAliasTableHandle,
                aliasTable,
                D3D12_RESOURCE_STATE_COPY_DEST);

            renderGraph.AddInput(data.PreLightingPassHandle,
                aliasTable,
                D3D12_RESOURCE_STATE_COPY_DEST);
        }

        // Direct + indirect lighting
        if (tlasReady)
        {
            // Lighting passes should run after alias table when it's recomputed
            if (!settings.LightPresampling && (App::GetScene().AreEmissivesStale() || data.EmissiveAliasTable.HasPendingRender()))
            {
                const uint32_t aliasTable = data.EmissiveAliasTable.GetOutput(
                    EmissiveTriangleAliasTable::SHADER_OUT_RES::ALIAS_TABLE).ID();

                renderGraph.AddInput(data.DirecLightingHandle,
                    aliasTable,
                    D3D12_RESOURCE_STATE_COPY_DEST);

                renderGraph.AddInput(data.IndirecLightingHandle,
                    aliasTable,
                    D3D12_RESOURCE_STATE_COPY_DEST);

                renderGraph.AddOutput(data.DirecLightingHandle,
                    data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            // Lighting passes should run after light presampling pass
            else if(settings.LightPresampling && presampledSetsBuiltOnce)
            {
                const uint32_t presampled = data.PreLightingPass.GePresampledSets().ID();

                renderGraph.AddOutput(data.PreLightingPassHandle,
                    presampled,
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

                renderGraph.AddInput(data.DirecLightingHandle,
                    presampled,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                renderGraph.AddInput(data.IndirecLightingHandle,
                    presampled,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                renderGraph.AddOutput(data.DirecLightingHandle,
                    data.DirecLightingPass.GetOutput(DirectLighting::SHADER_OUT_RES::DENOISED).ID(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
    }

    // Direct + indirect lighting depend on current and previous g-buffers
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
                gbuffData.Depth[1 - outIdx].ID(),
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

            renderGraph.AddInput(handles[i],
                gbuffData.IORBuffer[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.CoatBuffer[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.TriDiffGeo_A[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.TriDiffGeo_B[1 - outIdx].ID(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // Current g-buffers
            renderGraph.AddInput(handles[i],
                gbuffData.Normal[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.MetallicRoughness[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.Depth[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.MotionVec.ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.BaseColor[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.IORBuffer[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.CoatBuffer[outIdx].ID(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.TriDiffGeo_A[outIdx].ID(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            renderGraph.AddInput(handles[i],
                gbuffData.TriDiffGeo_B[outIdx].ID(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Outputs
        renderGraph.AddOutput(data.IndirecLightingHandle,
            data.IndirecLightingPass.GetOutput(IndirectLighting::SHADER_OUT_RES::DENOISED).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Sky DI
    if (numEmissives == 0 && tlasReady)
    {
        // Denoised output
        renderGraph.AddOutput(data.SkyDI_Handle,
            data.SkyDI_Pass.GetOutput(SkyDI::SHADER_OUT_RES::DENOISED).ID(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}
