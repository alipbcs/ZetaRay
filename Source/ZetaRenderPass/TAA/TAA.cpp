#include "TAA.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// TAA
//--------------------------------------------------------------------------------------

TAA::TAA()
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
        sizeof(cbTAA) / sizeof(DWORD),
        1);
}

TAA::~TAA()
{
    Reset();
}

void TAA::Init()
{
    D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto samplers = App::GetRenderer().GetStaticSamplers();
    RenderPassBase::InitRenderPass("TAA", flags, samplers);

    // use an arbitrary number as "nameID" since there's only one shader
    m_pso = m_psoLib.GetComputePSO(0, m_rootSigObj.Get(), COMPILED_CS[0]);

    m_descTable = App::GetRenderer().GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);
    CreateResources();

    m_localCB.BlendWeight = DefaultParamVals::BlendWeight;

    ParamVariant blendWeight;
    blendWeight.InitFloat("Renderer", "TAA", "BlendWeight", fastdelegate::MakeDelegate(this, &TAA::BlendWeightCallback),
        DefaultParamVals::BlendWeight,           // val
        0.0f,                                    // min
        1.0f,                                    // max
        0.1f);                                   // step
    App::AddParam(blendWeight);

    m_isTemporalTexValid = false;
    //App::AddShaderReloadHandler("TAA", fastdelegate::MakeDelegate(this, &TAA::ReloadShader));
}

void TAA::Reset()
{
    if (IsInitialized())
    {
        App::RemoveParam("Renderer", "TAA", "BlendWeight");
        App::RemoveShaderReloadHandler("TAA");

        m_antiAliased[0].Reset();
        m_antiAliased[1].Reset();

        m_descTable.Reset();
        m_pso = nullptr;

        RenderPassBase::ResetRenderPass();
    }
}

void TAA::OnWindowResized()
{
    CreateResources();
    m_isTemporalTexValid = false;
}

void TAA::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const int outIdx = renderer.GlobaIdxForDoubleBufferedResources();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderWidth();

    Assert(m_inputDesc[(int)SHADER_IN_DESC::SIGNAL] > 0, "Input SRV hasn't been set.");
    m_localCB.InputDescHeapIdx = m_inputDesc[(int)SHADER_IN_DESC::SIGNAL];
    m_localCB.PrevOutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex() + (outIdx == 0 ? (int)DESC_TABLE::TEX_A_SRV : (int)DESC_TABLE::TEX_B_SRV);
    m_localCB.CurrOutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex() + (outIdx == 0 ? (int)DESC_TABLE::TEX_B_UAV : (int)DESC_TABLE::TEX_A_UAV);
    m_localCB.TemporalIsValid = m_isTemporalTexValid;

    computeCmdList.PIXBeginEvent("TAA");

    // record the timestamp prior to execution
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "TAA");

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());
    computeCmdList.SetPipelineState(m_pso);

    m_rootSig.SetRootConstants(0, sizeof(cbTAA) / sizeof(DWORD), &m_localCB);
    m_rootSig.End(computeCmdList);

    computeCmdList.Dispatch(CeilUnsignedIntDiv(w, TAA_THREAD_GROUP_SIZE_X), 
        CeilUnsignedIntDiv(h, TAA_THREAD_GROUP_SIZE_Y), 1);

    computeCmdList.PIXEndEvent();

    // record the timestamp after execution
    gpuTimer.EndQuery(computeCmdList, queryIdx);

    m_isTemporalTexValid = true;
}

void TAA::CreateResources()
{
    auto& renderer = App::GetRenderer();

    m_antiAliased[0] = GpuMemory::GetTexture2D("TAA_A",
        renderer.GetRenderWidth(), renderer.GetRenderHeight(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_COMMON,
        CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    m_antiAliased[1] = GpuMemory::GetTexture2D("TAA_B",
        renderer.GetRenderWidth(), renderer.GetRenderHeight(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_COMMON,
        CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    // SRVs
    Direct3DUtil::CreateTexture2DSRV(m_antiAliased[0], m_descTable.CPUHandle((int)DESC_TABLE::TEX_A_SRV));
    Direct3DUtil::CreateTexture2DSRV(m_antiAliased[1], m_descTable.CPUHandle((int)DESC_TABLE::TEX_B_SRV));

    // UAVs
    Direct3DUtil::CreateTexture2DUAV(m_antiAliased[0], m_descTable.CPUHandle((int)DESC_TABLE::TEX_A_UAV));
    Direct3DUtil::CreateTexture2DUAV(m_antiAliased[1], m_descTable.CPUHandle((int)DESC_TABLE::TEX_B_UAV));
}

void TAA::BlendWeightCallback(const ParamVariant& p)
{
    m_localCB.BlendWeight = p.GetFloat().m_value;
}

void TAA::ReloadShader()
{
    m_psoLib.Reload(0, "TAA\\TAA.hlsl", true);
    m_pso = m_psoLib.GetComputePSO(0, m_rootSigObj.Get(), COMPILED_CS[0]);
}
