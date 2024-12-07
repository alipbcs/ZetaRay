#include "Compositing.h"
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Scene/SceneCore.h>
#include <Support/Param.h>
#include "../Assets/Font/IconsFontAwesome6.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// Compositing
//--------------------------------------------------------------------------------------

Compositing::Compositing()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // root constants
    m_rootSig.InitAsConstants(0, sizeof(cbCompositing) / sizeof(DWORD), 0);

    // frame constants
    m_rootSig.InitAsCBV(1, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);
}

void Compositing::InitPSOs()
{
    constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto& renderer = App::GetRenderer();
    auto samplers = renderer.GetStaticSamplers();
    RenderPassBase::InitRenderPass("Compositing", flags, samplers);

    for (int i = 0; i < (int)SHADER::COUNT; i++)
        m_psoLib.CompileComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void Compositing::Init()
{
    InitPSOs();

    memset(&m_cbComposit, 0, sizeof(m_cbComposit));
    SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::INDIRECT, true);

    CreateCompositTexture();

    ParamVariant p1;
    p1.InitBool(ICON_FA_FILM " Renderer", "Compositing", "Direct",
        fastdelegate::MakeDelegate(this, &Compositing::DirectCallback), m_directLighting);
    App::AddParam(p1);

    ParamVariant p6;
    p6.InitBool(ICON_FA_FILM " Renderer", "Compositing", "Indirect",
        fastdelegate::MakeDelegate(this, &Compositing::IndirectCallback),
        IS_CB_FLAG_SET(m_cbComposit, CB_COMPOSIT_FLAGS::INDIRECT));
    App::AddParam(p6);

    ParamVariant p9;
    p9.InitBool(ICON_FA_FILM " Renderer", "Compositing", "Firefly Suppression",
        fastdelegate::MakeDelegate(this, &Compositing::FireflyFilterCallback),
        m_filterFirefly);
    App::AddParam(p9);

    App::AddShaderReloadHandler("Compositing", fastdelegate::MakeDelegate(this, &Compositing::ReloadCompositing));
}

void Compositing::OnWindowResized()
{
    CreateCompositTexture();
}

void Compositing::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    const uint32_t w = App::GetRenderer().GetRenderWidth();
    const uint32_t h = App::GetRenderer().GetRenderHeight();
    auto& gpuTimer = App::GetRenderer().GetGpuTimer();

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    // compositing
    {
        computeCmdList.PIXBeginEvent("Compositing");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "Compositing");

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, COMPOSITING_THREAD_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, COMPOSITING_THREAD_GROUP_DIM_Y);

        if (IS_CB_FLAG_SET(m_cbComposit, CB_COMPOSIT_FLAGS::INSCATTERING))
        {
            Assert(m_cbComposit.InscatteringDescHeapIdx > 0, "Gpu descriptor for inscattering texture hasn't been set");
            Assert(m_cbComposit.VoxelGridNearZ >= 0.0f, "Invalid voxel grid depth");
            Assert(m_cbComposit.VoxelGridFarZ > m_cbComposit.VoxelGridNearZ, "Invalid voxel grid depth");
            Assert(m_cbComposit.DepthMappingExp > 0.0f, "Invalid voxel grid depth mapping exponent");
        }

        m_cbComposit.OutputUAVDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::LIGHT_ACCUM_UAV);

        m_rootSig.SetRootConstants(0, sizeof(cbCompositing) / sizeof(DWORD), &m_cbComposit);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::COMPOSIT));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }

    if (m_filterFirefly)
    {
        computeCmdList.PIXBeginEvent("FireflyFilter");
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "FireflyFilter");

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, FIREFLY_FILTER_THREAD_GROUP_DIM_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, FIREFLY_FILTER_THREAD_GROUP_DIM_Y);

        computeCmdList.UAVBarrier(m_compositTex.Resource());

        cbFireflyFilter cb;
        cb.CompositedUAVDescHeapIdx = m_descTable.GPUDescriptorHeapIndex((int)DESC_TABLE::LIGHT_ACCUM_UAV);

        m_rootSig.SetRootConstants(0, sizeof(cbFireflyFilter) / sizeof(DWORD), &cb);
        m_rootSig.End(computeCmdList);

        computeCmdList.SetPipelineState(m_psoLib.GetPSO((int)SHADER::FIREFLY_FILTER));
        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        gpuTimer.EndQuery(computeCmdList, queryIdx);
        computeCmdList.PIXEndEvent();
    }
}

void Compositing::CreateCompositTexture()
{
    auto& renderer = App::GetRenderer();
    m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);

    m_compositTex = GpuMemory::GetTexture2D("Composit",
        renderer.GetRenderWidth(), renderer.GetRenderHeight(),
        ResourceFormats::LIGHT_ACCUM,
        D3D12_RESOURCE_STATE_COMMON,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    Direct3DUtil::CreateTexture2DUAV(m_compositTex, m_descTable.CPUHandle((int)DESC_TABLE::LIGHT_ACCUM_UAV));
}

void Compositing::FireflyFilterCallback(const Support::ParamVariant& p)
{
    m_filterFirefly = p.GetBool();
}

void Compositing::DirectCallback(const Support::ParamVariant& p)
{
    m_directLighting = p.GetBool();

    if(App::GetScene().NumEmissiveInstances())
        SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::EMISSIVE_DI, m_directLighting);
    else
        SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::SKY_DI, m_directLighting);
}

void Compositing::IndirectCallback(const Support::ParamVariant& p)
{
    SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::INDIRECT, p.GetBool());
}

void Compositing::ReloadCompositing()
{
    const int i = (int)SHADER::COMPOSIT;
    m_psoLib.Reload(0, m_rootSigObj.Get(), "Compositing\\Compositing.hlsl");
}