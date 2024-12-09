#include "Display.h"
#include <Core/CommandList.h>
#include <Core/RenderGraph.h>
#include <Scene/SceneCore.h>
#include <Scene/Camera.h>
#include <Support/Param.h>
#include <Support/Task.h>
#include <Math/MatrixFuncs.h>
#include <App/Log.h>
#include "../Assets/Font/IconsFontAwesome6.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core::Direct3DUtil;

//--------------------------------------------------------------------------------------
// DisplayPass
//--------------------------------------------------------------------------------------

DisplayPass::DisplayPass()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // frame constants
    m_rootSig.InitAsCBV(0, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1, NUM_CONSTS, 1);
}

void DisplayPass::InitPSOs()
{
    constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto& renderer = App::GetRenderer();
    auto samplers = renderer.GetStaticSamplers();
    RenderPassBase::InitRenderPass("Display", flags, samplers);
    CreatePSOs();
}

void DisplayPass::Init()
{
    InitPSOs();

    memset(&m_cbLocal, 0, sizeof(m_cbLocal));
    m_cbLocal.DisplayOption = (uint16_t)DisplayOption::DEFAULT;
    m_cbLocal.Tonemapper = (uint16_t)Tonemapper::NEUTRAL;
    m_cbLocal.Saturation = 1.0f;
    m_cbLocal.AgXExp = 1.0f;
    m_cbLocal.RoughnessTh = 1.0f;
    m_cbLocal.AutoExposure = true;

    ParamVariant p1;
    p1.InitEnum(ICON_FA_FILM " Renderer", "Display", "Output", 
        fastdelegate::MakeDelegate(this, &DisplayPass::DisplayOptionCallback),
        Params::DisplayOptions, ZetaArrayLen(Params::DisplayOptions), m_cbLocal.DisplayOption);
    App::AddParam(p1);

    ParamVariant p2;
    p2.InitEnum(ICON_FA_FILM " Renderer", "Display", "View Transform", 
        fastdelegate::MakeDelegate(this, &DisplayPass::TonemapperCallback),
        Params::Tonemappers, ZetaArrayLen(Params::Tonemappers), m_cbLocal.Tonemapper);
    App::AddParam(p2);

    ParamVariant p3;
    p3.InitBool(ICON_FA_FILM " Renderer", "Auto Exposure", "Enable", 
        fastdelegate::MakeDelegate(this, &DisplayPass::AutoExposureCallback),
        m_cbLocal.AutoExposure);
    App::AddParam(p3);

    if (m_cbLocal.Tonemapper != (uint16_t)Tonemapper::AgX_DEFAULT &&
        m_cbLocal.Tonemapper != (uint16_t)Tonemapper::AgX_GOLDEN &&
        m_cbLocal.Tonemapper != (uint16_t)Tonemapper::AgX_PUNCHY)
    {
        ParamVariant p7;
        p7.InitFloat(ICON_FA_FILM " Renderer", "Display", "Saturation", 
            fastdelegate::MakeDelegate(this, &DisplayPass::SaturationCallback), 
            1, 0.5, 1.5f, 1e-2f);
        App::AddParam(p7);
    }

    if (m_cbLocal.Tonemapper == (uint16_t)Tonemapper::AgX_CUSTOM)
    {
        ParamVariant p4;
        p4.InitFloat(ICON_FA_FILM " Renderer", "Display", "Exponent",
            fastdelegate::MakeDelegate(this, &DisplayPass::AgxExpCallback),
            1, 0.0, 5.0f, 1e-2f);
        App::AddParam(p4);
    }

    App::Filesystem::Path p(App::GetAssetDir());
    p.Append("LUT\\tony_mc_mapface.dds");
    auto err = GpuMemory::GetTexture3DFromDisk(p.Get(), m_lut);
    Check(err == LOAD_DDS_RESULT::SUCCESS, "Error loading DDS texture from path %s: %d", 
        p.Get(), err);

    auto& renderer = App::GetRenderer();
    m_descTable = renderer.GetGpuDescriptorHeap().Allocate(DESC_TABLE::COUNT);
    Direct3DUtil::CreateTexture3DSRV(m_lut, m_descTable.CPUHandle(DESC_TABLE::TONEMAPPER_LUT_SRV));

    D3D12_CLEAR_VALUE clearVal;
    clearVal.Format = DXGI_FORMAT_R8_UNORM;
    clearVal.Color[0] = 0;
    clearVal.Color[1] = 0;
    clearVal.Color[2] = 0;
    clearVal.Color[3] = 0;
    m_pickMask = GpuMemory::GetTexture2D("PickMask", renderer.GetRenderWidth(), renderer.GetRenderHeight(),
        DXGI_FORMAT_R8_UNORM, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, 
        TEXTURE_FLAGS::ALLOW_RENDER_TARGET, 1, &clearVal);
    m_rtvDescTable = renderer.GetRtvDescriptorHeap().Allocate(1);
    Direct3DUtil::CreateRTV(m_pickMask, m_rtvDescTable.CPUHandle(0));
    Direct3DUtil::CreateTexture2DSRV(m_pickMask, m_descTable.CPUHandle(DESC_TABLE::PICK_MASK_SRV));
}

void DisplayPass::SetPickData(const Core::RenderNodeHandle& producerHandle,
    Core::GpuMemory::ReadbackHeapBuffer* readback,
    fastdelegate::FastDelegate0<> dlg)
{
    Assert(producerHandle.IsValid(), "Invalid handle.");
    Assert(readback, "Readback buffer was NULL.");
    m_producerHandle = producerHandle.Val;
    m_readback = readback;
    m_pickDlg = dlg;
}

void DisplayPass::ClearPick()
{
    App::RemoveParam(ICON_FA_FILM " Renderer", "Display", "Wireframe");
}

void DisplayPass::CaptureScreen()
{
    Assert(!m_captureScreen, "Duplicate call.");

    auto& renderer = App::GetRenderer();
    auto* device = renderer.GetDevice();
    auto& backBuffer = renderer.GetCurrentBackBuffer();
    auto desc = backBuffer.Desc();

    UINT64 totalResourceSize = 0;
    UINT64 rowSizeInBytes = 0;
    UINT rowCount = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr,
        &rowCount, &rowSizeInBytes, &totalResourceSize);

    // From MS docs: "... a Texture2D resource has a width of 32 and bytes per pixel of 4,
    // then pRowSizeInBytes returns 128. pRowSizeInBytes should not be confused with row pitch, 
    // as examining pLayouts and getting the row pitch from that will give you 256 as it is 
    // aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT."
    const uint32_t rowPitch = (uint32_t)Math::AlignUp(rowSizeInBytes, 
        (uint64_t)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t sizeInBytes = rowPitch * desc.Height;
    m_screenCaptureReadback = GpuMemory::GetReadbackHeapBuffer(sizeInBytes);

    m_backBufferFoorprint.Format = desc.Format;
    m_backBufferFoorprint.Width = (UINT)desc.Width;
    m_backBufferFoorprint.Height = (UINT)desc.Height;
    m_backBufferFoorprint.Depth = 1;
    m_backBufferFoorprint.RowPitch = rowPitch;

    m_captureScreen = true;
}

void DisplayPass::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
    GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

    Assert(m_compositedSrvDescHeapIdx != UINT32_MAX, "Gpu Desc Idx hasn't been set.");
    Assert(m_cbLocal.ExposureDescHeapIdx > 0, "Gpu Desc Idx hasn't been set.");

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    auto& scene = App::GetScene();

    directCmdList.PIXBeginEvent("Display");
    const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "Display");

    directCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());
    directCmdList.SetPipelineState(m_psoLib.GetPSO((int)DISPLAY_SHADER::DISPLAY));

    m_cbLocal.LUTDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::TONEMAPPER_LUT_SRV);
    m_cbLocal.InputDescHeapIdx = m_compositedSrvDescHeapIdx;
    m_rootSig.SetRootConstants(0, sizeof(cbDisplayPass) / sizeof(DWORD), &m_cbLocal);
    m_rootSig.End(directCmdList);

    if (RenderNodeHandle(m_producerHandle).IsValid())
    {
        const uint64_t fence = scene.GetRenderGraph()->GetCompletionFence(
            RenderNodeHandle(m_producerHandle));
        Assert(fence != UINT64_MAX, "Invalid fence value.");

        // Wait on a background thread for GPU to finish copying to readback buffer
        Task t("WaitForGBuffer", TASK_PRIORITY::BACKGROUND, [this, fence]()
            {
                //HANDLE fenceEvent = CreateEventA(nullptr, false, false, nullptr);
                //CheckWin32(fenceEvent);

                App::GetRenderer().WaitForDirectQueueFenceCPU(fence);
                //App::GetRenderer().WaitForDirectQueueFenceCPU(fence, fenceEvent);
                //CloseHandle(fenceEvent);

                ReadbackPickIdx();
            });

        App::SubmitBackground(ZetaMove(t));

        m_producerHandle = RenderNodeHandle::INVALID_HANDLE;
        m_pickDlg();
    }

    D3D12_VIEWPORT viewports[1] = { renderer.GetDisplayViewport() };
    D3D12_RECT scissors[1] = { renderer.GetDisplayScissor() };
    directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
    Assert(m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
    directCmdList.OMSetRenderTargets(1, &m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV], true, nullptr);
    directCmdList.DrawInstanced(3, 1, 0, 0);

    if (auto picks = scene.GetPickedInstances().m_span; !picks.empty())
        DrawPicked(directCmdList, picks);

    if (m_captureScreen)
    {
        ID3D12Resource* backbuffer = const_cast<Texture&>(renderer.GetCurrentBackBuffer()).Resource();
        directCmdList.ResourceBarrier(backbuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = backbuffer;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = m_screenCaptureReadback.Resource();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLocation.PlacedFootprint.Footprint = m_backBufferFoorprint;

        // Copy the texture
        directCmdList.CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

        directCmdList.ResourceBarrier(backbuffer,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Wait on a background thread for GPU to finish copying to readback buffer
        Task t("WaitForCapture", TASK_PRIORITY::BACKGROUND, [this]()
            {
                WaitObject waitObj;
                App::GetScene().GetRenderGraph()->SetFrameSubmissionWaitObj(waitObj);
                waitObj.Wait();

                const uint64_t fence = App::GetScene().GetRenderGraph()->GetFrameCompletionFence();
                Assert(fence != UINT64_MAX, "Invalid fence value.");

                App::GetRenderer().WaitForDirectQueueFenceCPU(fence);
                ReadbackScreenCapture();
            });

        App::SubmitBackground(ZetaMove(t));

        m_captureScreen = false;
    }

    gpuTimer.EndQuery(directCmdList, queryIdx);
    directCmdList.PIXEndEvent();
}

void DisplayPass::DrawPicked(GraphicsCmdList& cmdList, Span<uint64_t> picks)
{
    Assert(!picks.empty(), "Invalid argument.");
    const auto& camera = App::GetCamera();
    auto& frustum = camera.GetCameraFrustumViewSpace();
    auto viewInv = camera.GetViewInv();

    // Transform view frustum from view space into world space
    v_float4x4 vM = load4x4(const_cast<float4x4a&>(viewInv));
    v_ViewFrustum vFrustum(const_cast<ViewFrustum&>(frustum));
    vFrustum = transform(vM, vFrustum);

    for (auto ID : picks)
    {
        v_AABB vBox(App::GetScene().GetAABB(ID));
        v_float4x4 vW = load4x3(App::GetScene().GetToWorld(ID));
        vBox = transform(vW, vBox);

        // Skip if outside the view frustum
        if (instersectFrustumVsAABB(vFrustum, vBox) == COLLISION_TYPE::DISJOINT)
            continue;

        // Draw mask
        {
            auto& scene = App::GetScene();
            auto meshID = scene.GetInstanceMeshID(ID);
            auto* mesh = scene.GetMesh(meshID).value();
            float4x3 toWorld = scene.GetToWorld(ID);

            const Camera& cam = App::GetCamera();
            v_float4x4 vView = load4x4(const_cast<float4x4a&>(cam.GetCurrView()));
            v_float4x4 vProj = load4x4(const_cast<float4x4a&>(cam.GetProj()));
            v_float4x4 vVP = mul(vView, vProj);
            v_float4x4 vW2 = load4x3(toWorld);
            v_float4x4 vWVP = mul(vW2, vVP);
            float4x4a wvp = store(vWVP);

            cbDrawPicked cb;
            cb.row0 = wvp.m[0];
            cb.row1 = wvp.m[1];
            cb.row2 = wvp.m[2];
            cb.row3 = wvp.m[3];

            const Buffer& sceneVB = App::GetScene().GetMeshVB();
            const Buffer& sceneIB = App::GetScene().GetMeshIB();

            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.StrideInBytes = sizeof(Vertex);
            vbv.BufferLocation = sceneVB.GpuVA() + mesh->m_vtxBuffStartOffset * sizeof(Vertex);
            vbv.SizeInBytes = mesh->m_numVertices * sizeof(Vertex);

            D3D12_INDEX_BUFFER_VIEW ibv;
            ibv.Format = DXGI_FORMAT_R32_UINT;
            ibv.BufferLocation = sceneIB.GpuVA() + mesh->m_idxBuffStartOffset * sizeof(uint32);
            ibv.SizeInBytes = mesh->m_numIndices * sizeof(uint32);

            auto layoutToRT = TextureBarrier(m_pickMask.Resource(),
                D3D12_BARRIER_SYNC_NONE,
                D3D12_BARRIER_SYNC_DRAW,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_ACCESS_RENDER_TARGET);
            cmdList.ResourceBarrier(layoutToRT);

            auto* pso = m_wireframe ? m_psoLib.GetPSO((int)DISPLAY_SHADER::DRAW_PICKED_WIREFRAME) :
                m_psoLib.GetPSO((int)DISPLAY_SHADER::DRAW_PICKED);
            cmdList.SetPipelineState(pso);
            cmdList.IASetVertexAndIndexBuffers(vbv, ibv);
            auto cpuHandle = m_rtvDescTable.CPUHandle(0);
            cmdList.OMSetRenderTargets(1, &cpuHandle, true, nullptr);

            cmdList.ClearRenderTargetView(cpuHandle, 0, 0, 0, 0);

            m_rootSig.SetRootConstants(0, sizeof(cbDrawPicked) / sizeof(uint32), &cb);
            m_rootSig.End(cmdList);

            cmdList.DrawIndexedInstanced(mesh->m_numIndices,
                1,
                0,
                0,
                0);
        }

        // Sobel
        {
            cmdList.SetPipelineState(m_psoLib.GetPSO((int)DISPLAY_SHADER::SOBEL));
            cmdList.OMSetRenderTargets(1, &m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV], true, nullptr);

            auto syncDrawAndLayoutToRead = TextureBarrier(m_pickMask.Resource(),
                D3D12_BARRIER_SYNC_DRAW,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,
                D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                D3D12_BARRIER_ACCESS_RENDER_TARGET,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            cmdList.ResourceBarrier(syncDrawAndLayoutToRead);

            cbSobel cb;
            cb.MaskDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::PICK_MASK_SRV);
            cb.Wireframe = m_wireframe;

            m_rootSig.SetRootConstants(0, sizeof(cbSobel) / sizeof(uint32), &cb);
            m_rootSig.End(cmdList);

            cmdList.DrawInstanced(3, 1, 0, 0);
        }
    }
}

void DisplayPass::CreatePSOs()
{
    // Display
    {
        DXGI_FORMAT rtvFormats[1] = { Constants::BACK_BUFFER_FORMAT };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DUtil::GetPSODesc(nullptr,
            1, rtvFormats);

        // no blending required

        // disable depth testing and writing
        psoDesc.DepthStencilState.DepthEnable = false;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

        // disable triangle culling
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        m_psoLib.CompileGraphicsPSO((int)DISPLAY_SHADER::DISPLAY,
            psoDesc,
            m_rootSigObj.Get(),
            COMPILED_VS[(int)DISPLAY_SHADER::DISPLAY],
            COMPILED_PS[(int)DISPLAY_SHADER::DISPLAY]);
    }

    // Draw mask
    {
        D3D12_INPUT_ELEMENT_DESC inputElements[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, 
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, 
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R16G16_SINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, 
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R16G16_SINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, 
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_INPUT_LAYOUT_DESC inputLayout = D3D12_INPUT_LAYOUT_DESC{ .pInputElementDescs = inputElements, 
            .NumElements = ZetaArrayLen(inputElements) };

        DXGI_FORMAT rtvFormats[1] = { DXGI_FORMAT_R8_UNORM };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DUtil::GetPSODesc(&inputLayout,
            1, rtvFormats);

        // disable triangle culling
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        // disable depth testing and writing
        psoDesc.DepthStencilState.DepthEnable = false;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

        m_psoLib.CompileGraphicsPSO((int)DISPLAY_SHADER::DRAW_PICKED,
            psoDesc,
            m_rootSigObj.Get(),
            COMPILED_VS[(int)DISPLAY_SHADER::DRAW_PICKED],
            COMPILED_PS[(int)DISPLAY_SHADER::DRAW_PICKED]);

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

        m_psoLib.CompileGraphicsPSO((int)DISPLAY_SHADER::DRAW_PICKED_WIREFRAME,
            psoDesc,
            m_rootSigObj.Get(),
            COMPILED_VS[(int)DISPLAY_SHADER::DRAW_PICKED],
            COMPILED_PS[(int)DISPLAY_SHADER::DRAW_PICKED]);
    }

    // Sobel
    {
        DXGI_FORMAT rtvFormats[1] = { Constants::BACK_BUFFER_FORMAT };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DUtil::GetPSODesc(nullptr,
            1, rtvFormats);

        // disable depth testing and writing
        psoDesc.DepthStencilState.DepthEnable = false;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

        // disable triangle culling
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        m_psoLib.CompileGraphicsPSO((int)DISPLAY_SHADER::SOBEL,
            psoDesc,
            m_rootSigObj.Get(),
            COMPILED_VS[(int)DISPLAY_SHADER::SOBEL],
            COMPILED_PS[(int)DISPLAY_SHADER::SOBEL]);
    }
}

void DisplayPass::ReadbackPickIdx()
{
    Assert(m_readback, "Readback buffer hasn't been set.");
    auto& scene = App::GetScene();
    auto pickWasDisabled = scene.GetPickedInstances().m_span.empty();
    m_readback->Map();

    uint32* data = reinterpret_cast<uint32*>(m_readback->MappedMemory());
    uint32 rtMeshIdx;
    memcpy(&rtMeshIdx, data, sizeof(uint32));

    m_readback->Unmap();
    m_readback = nullptr;

    if (rtMeshIdx == UINT32_MAX)
        return;

    auto id = scene.GetIDFromRtMeshIdx(rtMeshIdx);
    scene.SetPickedInstance(id);

    if (pickWasDisabled)
    {
        ParamVariant p;
        p.InitBool(ICON_FA_FILM " Renderer", "Display", "Wireframe",
            fastdelegate::MakeDelegate(this, &DisplayPass::WireframeCallback),
            m_wireframe);
        App::AddParam(p);
    }
}

void DisplayPass::ReadbackScreenCapture()
{
    Assert(m_screenCaptureReadback.IsInitialized(), "Readback buffer hasn't been initialized.");

    m_screenCaptureReadback.Map();
    uint32_t* data = reinterpret_cast<uint32_t*>(m_screenCaptureReadback.MappedMemory());

    SYSTEMTIME st;
    GetLocalTime(&st);
    StackStr(currTime, N1, "%u_%u_%u_%u_%u_%u", st.wYear, st.wMonth, st.wDay, 
        st.wHour, st.wMinute, st.wSecond);

    uint32_t hash = Util::XXH3_64_To_32(XXH3_64bits(currTime, N1));
    StackStr(filename, N2, "capture_%u.png", hash);

    int res = stbi_write_png(filename, m_backBufferFoorprint.Width, m_backBufferFoorprint.Height,
        4, data, m_backBufferFoorprint.RowPitch);
    Check(res != 0, "stbi_write_png() failed.");

    m_screenCaptureReadback.Unmap();
    m_screenCaptureReadback.Reset(false);

    LOG_UI_INFO("Screenshot saved to: %s.\n", filename);
}

void DisplayPass::DisplayOptionCallback(const ParamVariant& p)
{
    m_cbLocal.DisplayOption = (uint16_t)p.GetEnum().m_curr;

    if (m_cbLocal.DisplayOption == (uint32)DisplayOption::ROUGHNESS_TH)
    {
        ParamVariant p1;
        p1.InitFloat(ICON_FA_FILM " Renderer", "Display", "Roughness (Th)",
            fastdelegate::MakeDelegate(this, &DisplayPass::RoughnessThCallback),
            1, 0.0, 1.0f, 1e-2f);
        App::AddParam(p1);
    }
    else
        App::RemoveParam(ICON_FA_FILM " Renderer", "Display", "Roughness (Th)");
}

void DisplayPass::TonemapperCallback(const Support::ParamVariant& p)
{
    m_cbLocal.Tonemapper = (uint16_t)p.GetEnum().m_curr;

    if (m_cbLocal.Tonemapper == (uint16_t)Tonemapper::AgX_CUSTOM)
    {
        ParamVariant p1;
        p1.InitFloat(ICON_FA_FILM " Renderer", "Display", "Exponent",
            fastdelegate::MakeDelegate(this, &DisplayPass::AgxExpCallback),
            1, 0.0, 5.0f, 1e-2f);
        App::AddParam(p1);
    }
    else
        App::RemoveParam(ICON_FA_FILM " Renderer", "Display", "Exponent");

    if (m_cbLocal.Tonemapper == (uint16_t)Tonemapper::AgX_DEFAULT ||
        m_cbLocal.Tonemapper == (uint16_t)Tonemapper::AgX_GOLDEN ||
        m_cbLocal.Tonemapper == (uint16_t)Tonemapper::AgX_PUNCHY)
    {
        App::RemoveParam(ICON_FA_FILM " Renderer", "Display", "Saturation");
    }
    else
    {
        ParamVariant p2;
        p2.InitFloat(ICON_FA_FILM " Renderer", "Display", "Saturation",
            fastdelegate::MakeDelegate(this, &DisplayPass::SaturationCallback),
            1, 0.5, 1.5f, 1e-2f);
        App::TryAddParam(p2);
    }
}

void DisplayPass::SaturationCallback(const Support::ParamVariant& p)
{
    m_cbLocal.Saturation = p.GetFloat().m_value;
}

void DisplayPass::AgxExpCallback(const Support::ParamVariant& p)
{
    m_cbLocal.AgXExp = p.GetFloat().m_value;
}

void DisplayPass::AutoExposureCallback(const Support::ParamVariant& p)
{
    m_cbLocal.AutoExposure = p.GetBool();
}

void DisplayPass::RoughnessThCallback(const Support::ParamVariant& p)
{
    m_cbLocal.RoughnessTh = p.GetFloat().m_value;
}

void DisplayPass::WireframeCallback(const Support::ParamVariant& p)
{
    m_wireframe = p.GetBool();
}
