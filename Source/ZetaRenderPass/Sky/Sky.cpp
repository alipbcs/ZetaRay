#include "Sky.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneCore.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;

//--------------------------------------------------------------------------------------
// Sky
//--------------------------------------------------------------------------------------

Sky::Sky()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // root constants
    m_rootSig.InitAsConstants(0,
        NUM_CONSTS,
        0);

    // frame constants
    m_rootSig.InitAsCBV(1,
        1,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // BVH
    m_rootSig.InitAsBufferSRV(2,
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH);
}

Sky::~Sky()
{
    Reset();
}

void Sky::Init(int lutWidth, int lutHeight, bool doInscattering)
{
    Assert(lutHeight > 0 && lutWidth > 0, "invalid texture dimensions");

    m_localCB.LutWidth = lutWidth;
    m_localCB.LutHeight = lutHeight;

    auto& renderer = App::GetRenderer();

    D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto samplers = renderer.GetStaticSamplers();
    RenderPassBase::InitRenderPass("Sky", flags, samplers);

    m_psos[(int)SHADERS::SKY_LUT] = m_psoLib.GetComputePSO((int)SHADERS::SKY_LUT,
        m_rootSigObj.Get(), COMPILED_CS[(int)SHADERS::SKY_LUT]);

    m_descTable = renderer.GetGpuDescriptorHeap().Allocate((int)DESC_TABLE::COUNT);

    m_localCB.DepthMappingExp = DefaultParamVals::DEPTH_MAP_EXP;
    m_localCB.VoxelGridNearZ = DefaultParamVals::VOXEL_GRID_NEAR_Z;
    m_localCB.VoxelGridFarZ = DefaultParamVals::VOXEL_GRID_FAR_Z;
    m_localCB.NumVoxelsX = DefaultParamVals::NUM_VOXELS_X;
    m_localCB.NumVoxelsY = DefaultParamVals::NUM_VOXELS_Y;

    CreateSkyviewLUT();
    App::AddShaderReloadHandler("SkyViewLUT", fastdelegate::MakeDelegate(this, &Sky::ReloadSkyLUTShader));

    SetInscatteringEnablement(doInscattering);
}

void Sky::Reset()
{
    if (IsInitialized())
    {
        m_descTable.Reset();
        m_lut.Reset();
        SetInscatteringEnablement(false);

        RenderPassBase::ResetRenderPass();
    }
}

void Sky::SetInscatteringEnablement(bool b)
{
    if (b == m_doInscattering)
        return;

    m_doInscattering = b;

    if (b)
    {
        Assert(!m_voxelGrid.IsInitialized(), "This should be NULL");

        CreateVoxelGrid();

        ParamVariant depthExp;
        depthExp.InitFloat("Renderer", "Inscattering", "DepthMapExp", fastdelegate::MakeDelegate(this, &Sky::DepthMapExpCallback),
            DefaultParamVals::DEPTH_MAP_EXP,        // val
            1.0f,                                   // min
            5.0f,                                   // max
            0.2f);                                  // step
        App::AddParam(depthExp);

        ParamVariant voxelGridNearZ;
        voxelGridNearZ.InitFloat("Renderer", "Inscattering", "VoxelGridNearZ", fastdelegate::MakeDelegate(this, &Sky::VoxelGridNearZCallback),
            DefaultParamVals::VOXEL_GRID_NEAR_Z,    // val
            0.0f,                                   // min
            1.0f,                                   // max
            1e-2f);                                 // step
        App::AddParam(voxelGridNearZ);

        ParamVariant voxelGridFarZ;
        voxelGridFarZ.InitFloat("Renderer", "Inscattering", "VoxelGridFarZ", fastdelegate::MakeDelegate(this, &Sky::VoxelGridFarZCallback),
            DefaultParamVals::VOXEL_GRID_FAR_Z,       // val
            10.0f,                                    // min
            200.0f,                                   // max
            1.0f);                                    // step
        App::AddParam(voxelGridFarZ);

        //App::AddShaderReloadHandler("Inscattering", fastdelegate::MakeDelegate(this, &Sky::ReloadInscatteringShader));

        m_psos[(int)SHADERS::INSCATTERING] = m_psoLib.GetComputePSO((int)SHADERS::INSCATTERING,
            m_rootSigObj.Get(), COMPILED_CS[(int)SHADERS::INSCATTERING]);
    }
    else
    {
        m_voxelGrid.Reset();

        App::RemoveParam("Renderer", "Inscattering", "DepthMapExp");
        App::RemoveParam("Renderer", "Inscattering", "VoxelGridNearZ");
        App::RemoveParam("Renderer", "Inscattering", "VoxelGridFarZ");

        //App::RemoveShaderReloadHandler("Inscattering");

        m_psos[(int)SHADERS::INSCATTERING] = nullptr;
    }
}

void Sky::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());
    m_rootSig.SetRootConstants(0, sizeof(m_localCB) / sizeof(DWORD), &m_localCB);
    m_rootSig.End(computeCmdList);

    //
    // Sky LUT
    //
    {
        computeCmdList.PIXBeginEvent("SkyViewLUT");

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "SkyViewLUT");

        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::SKY_LUT]);

        const uint32_t dispatchDimX = CeilUnsignedIntDiv(m_localCB.LutWidth, SKY_VIEW_LUT_THREAD_GROUP_SIZE_X);
        const uint32_t dispatchDimY = CeilUnsignedIntDiv(m_localCB.LutHeight, SKY_VIEW_LUT_THREAD_GROUP_SIZE_Y);

        computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        computeCmdList.PIXEndEvent();
    }

    //
    // Inscattering
    //

    if(m_doInscattering)
    {
        computeCmdList.PIXBeginEvent("InscatteringVoxelGrid");

        // record the timestamp prior to execution
        const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "InscatteringVoxelGrid");

        computeCmdList.SetPipelineState(m_psos[(int)SHADERS::INSCATTERING]);
        computeCmdList.Dispatch(m_localCB.NumVoxelsX, m_localCB.NumVoxelsY, 1);

        // record the timestamp after execution
        gpuTimer.EndQuery(computeCmdList, queryIdx);

        computeCmdList.PIXEndEvent();
    }
}

void Sky::CreateSkyviewLUT()
{
    auto& renderer = App::GetRenderer();
    auto* device = renderer.GetDevice();

    m_lut = GpuMemory::GetTexture2D("SkyLUT",
        m_localCB.LutWidth, m_localCB.LutHeight,
        ResourceFormats::SKY_VIEW_LUT,
        D3D12_RESOURCE_STATE_COMMON,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = ResourceFormats::SKY_VIEW_LUT;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;

    device->CreateUnorderedAccessView(m_lut.Resource(), nullptr, &uavDesc, m_descTable.CPUHandle((int)DESC_TABLE::SKY_LUT_UAV));

    m_localCB.LutDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::SKY_LUT_UAV);
}

void Sky::CreateVoxelGrid()
{
    auto* device = App::GetRenderer().GetDevice();

    m_voxelGrid = GpuMemory::GetTexture3D("InscatteringVoxelGrid",
        m_localCB.NumVoxelsX, m_localCB.NumVoxelsY, INSCATTERING_THREAD_GROUP_SIZE_X,
        ResourceFormats::INSCATTERING_VOXEL_GRID,
        D3D12_RESOURCE_STATE_COMMON,
        TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Format = ResourceFormats::INSCATTERING_VOXEL_GRID;
    uavDesc.Texture3D.MipSlice = 0;
    uavDesc.Texture3D.WSize = INSCATTERING_THREAD_GROUP_SIZE_X;
    uavDesc.Texture3D.FirstWSlice = 0;

    device->CreateUnorderedAccessView(m_voxelGrid.Resource(), nullptr, &uavDesc, m_descTable.CPUHandle((int)DESC_TABLE::VOXEL_GRID_UAV));

    m_localCB.VoxelGridDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::VOXEL_GRID_UAV);    
}

void Sky::DepthMapExpCallback(const ParamVariant& p)
{
    m_localCB.DepthMappingExp = p.GetFloat().m_value;
}

void Sky::VoxelGridNearZCallback(const ParamVariant& p)
{
    m_localCB.VoxelGridNearZ = p.GetFloat().m_value;
}

void Sky::VoxelGridFarZCallback(const ParamVariant& p)
{
    m_localCB.VoxelGridFarZ = p.GetFloat().m_value;
}

void Sky::ReloadInscatteringShader()
{
    m_psoLib.Reload((int)SHADERS::INSCATTERING, "Sky\\Inscattering.hlsl", true);
    m_psos[(int)SHADERS::INSCATTERING] = m_psoLib.GetComputePSO((int)SHADERS::INSCATTERING, 
        m_rootSigObj.Get(), COMPILED_CS[(int)SHADERS::INSCATTERING]);
}

void Sky::ReloadSkyLUTShader()
{
    m_psoLib.Reload((int)SHADERS::SKY_LUT, "Sky\\SkyViewLUT.hlsl", true);
    m_psos[(int)SHADERS::SKY_LUT] = m_psoLib.GetComputePSO((int)SHADERS::SKY_LUT,
        m_rootSigObj.Get(), COMPILED_CS[(int)SHADERS::SKY_LUT]);
}
