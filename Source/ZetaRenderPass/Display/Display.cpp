#include "Display.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;
using namespace ZetaRay::Core::Direct3DUtil;

//--------------------------------------------------------------------------------------
// DisplayPass
//--------------------------------------------------------------------------------------

DisplayPass::DisplayPass()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // frame constants
    m_rootSig.InitAsCBV(0,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1,
        NUM_CONSTS,
        1);
}

DisplayPass::~DisplayPass()
{
    Reset();
}

void DisplayPass::Init()
{
    auto& renderer = App::GetRenderer();

    D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto samplers = renderer.GetStaticSamplers();
    RenderPassBase::InitRenderPass("Display", flags, samplers);
    CreatePSOs();

    memset(&m_cbLocal, 0, sizeof(m_cbLocal));
    m_cbLocal.DisplayOption = (uint16_t)DisplayOption::DEFAULT;
    m_cbLocal.Tonemapper = (uint16_t)Tonemapper::NEUTRAL;
    m_cbLocal.Saturation = 1.0f;
    m_cbLocal.AutoExposure = true;

    ParamVariant p1;
    p1.InitEnum("Renderer", "Display", "Final Render", fastdelegate::MakeDelegate(this, &DisplayPass::DisplayOptionCallback),
        Params::DisplayOptions, ZetaArrayLen(Params::DisplayOptions), m_cbLocal.DisplayOption);
    App::AddParam(p1);

    ParamVariant p2;
    p2.InitEnum("Renderer", "Display", "Tonemapper", fastdelegate::MakeDelegate(this, &DisplayPass::TonemapperCallback),
        Params::Tonemappers, ZetaArrayLen(Params::Tonemappers), m_cbLocal.Tonemapper);
    App::AddParam(p2);

    ParamVariant p3;
    p3.InitBool("Renderer", "Auto Exposure", "Enable", fastdelegate::MakeDelegate(this, &DisplayPass::AutoExposureCallback),
        m_cbLocal.AutoExposure);
    App::AddParam(p3);

    ParamVariant p7;
    p7.InitFloat("Renderer", "Display", "Saturation", fastdelegate::MakeDelegate(this, &DisplayPass::SaturationCallback), 
        1, 0.5, 1.5f, 1e-2f);
    App::AddParam(p7);

    App::Filesystem::Path p(App::GetAssetDir());
    p.Append("LUT\\tony_mc_mapface.dds");
    auto err = GpuMemory::GetTexture3DFromDisk(p, m_lut);
    Check(err == LOAD_DDS_RESULT::SUCCESS, "Error while loading DDS texture from path %s: %d", p.Get(), err);

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate(DESC_TABLE::COUNT);
    Direct3DUtil::CreateTexture3DSRV(m_lut, m_descTable.CPUHandle(DESC_TABLE::TONEMAPPER_LUT_SRV));
}

void DisplayPass::Reset()
{
    if (IsInitialized())
        RenderPassBase::ResetRenderPass();
}

void DisplayPass::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
    GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

    Assert(m_compositedSrvDescHeapIdx != uint32_t(-1), "Gpu Desc Idx hasn't been set.");
    Assert(m_cbLocal.ExposureDescHeapIdx > 0, "Gpu Desc Idx hasn't been set.");

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();

    directCmdList.PIXBeginEvent("Display");

    // record the timestamp prior to execution
    const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "Display");

    directCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());
    directCmdList.SetPipelineState(m_psosPS[(int)PS_SHADERS::DISPLAY]);

    m_cbLocal.LUTDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::TONEMAPPER_LUT_SRV);
    m_cbLocal.InputDescHeapIdx = m_compositedSrvDescHeapIdx;
    m_rootSig.SetRootConstants(0, sizeof(cbDisplayPass) / sizeof(DWORD), &m_cbLocal);
    m_rootSig.End(directCmdList);

    D3D12_VIEWPORT viewports[1] = { renderer.GetDisplayViewport() };
    D3D12_RECT scissors[1] = { renderer.GetDisplayScissor() };
    directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    directCmdList.RSSetViewportsScissorsRects(1, viewports, scissors);
    Assert(m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
    directCmdList.OMSetRenderTargets(1, &m_cpuDescs[(int)SHADER_IN_CPU_DESC::RTV], true, nullptr);
    directCmdList.DrawInstanced(3, 1, 0, 0);

    // record the timestamp after execution
    gpuTimer.EndQuery(directCmdList, queryIdx);

    directCmdList.PIXEndEvent();
}

void DisplayPass::CreatePSOs()
{
    DXGI_FORMAT rtvFormats[1] = { Constants::BACK_BUFFER_FORMAT };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DUtil::GetPSODesc(nullptr,
        1,
        rtvFormats);

    // no blending required

    // disable depth testing and writing
    psoDesc.DepthStencilState.DepthEnable = false;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    // disable triangle culling
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    m_psosPS[(int)PS_SHADERS::DISPLAY] = m_psoLib.GetGraphicsPSO((int)PS_SHADERS::DISPLAY,
        psoDesc,
        m_rootSigObj.Get(),
        COMPILED_VS[(int)PS_SHADERS::DISPLAY],
        COMPILED_PS[(int)PS_SHADERS::DISPLAY]);
}

void DisplayPass::DisplayOptionCallback(const ParamVariant& p)
{
    m_cbLocal.DisplayOption = (uint16_t)p.GetEnum().m_curr;
}

void DisplayPass::TonemapperCallback(const Support::ParamVariant& p)
{
    m_cbLocal.Tonemapper = (uint16_t)p.GetEnum().m_curr;
}

void DisplayPass::SaturationCallback(const Support::ParamVariant& p)
{
    m_cbLocal.Saturation = p.GetFloat().m_val;
}

void DisplayPass::AutoExposureCallback(const Support::ParamVariant& p)
{
    m_cbLocal.AutoExposure = p.GetBool();
}
