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

#define GAUSSIAN_FILTER 1

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
	m_cbDoF.FocusDepth = 5.0f;
	m_cbDoF.FocalLength = 50;
	m_cbDoF.FStop = 1.4f;
	m_cbDoF.MaxBlurRadius = 16.0f;
	m_cbDoF.RadiusScale = 1.6f;
	m_cbDoF.MinLumToFilter = 1.0f;

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

	ParamVariant dof;
	dof.InitBool("Renderer", "DoF", "Enable", fastdelegate::MakeDelegate(this, &DisplayPass::SetDoFEnablement),
		m_dof);
	App::AddParam(dof);

	ParamVariant focusDist;
	focusDist.InitFloat("Renderer", "DoF", "Focus Distance",
		fastdelegate::MakeDelegate(this, &DisplayPass::FocusDistCallback),
		m_cbDoF.FocusDepth,			// val	
		0.1f,								// min
		25.0f,								// max
		1e-3f);								// step
	App::AddParam(focusDist);

	ParamVariant fstop;
	fstop.InitFloat("Renderer", "DoF", "F-Stop",
		fastdelegate::MakeDelegate(this, &DisplayPass::FStopCallback),
		m_cbDoF.FStop,					// val	
		1e-1f,								// min
		5.0f,								// max
		1e-2f);								// step
	App::AddParam(fstop);

	ParamVariant focalLen;
	focalLen.InitFloat("Renderer", "DoF", "Focal Length (mm)",
		fastdelegate::MakeDelegate(this, &DisplayPass::FocalLengthCallback),
		m_cbDoF.FocalLength,			// val	
		1.0f,								// min
		100.0f,								// max
		1e-1f);								// step
	App::AddParam(focalLen);

	ParamVariant blurRadius;
	blurRadius.InitFloat("Renderer", "DoF", "Max Blur Radius",
		fastdelegate::MakeDelegate(this, &DisplayPass::BlurRadiusCallback),
		m_cbDoF.MaxBlurRadius,				// val	
		5.0f,								// min
		25.0f,								// max
		1e-1f);								// step
	App::AddParam(blurRadius);

	ParamVariant radiusScale;
	radiusScale.InitFloat("Renderer", "DoF", "Radius Scale",
		fastdelegate::MakeDelegate(this, &DisplayPass::RadiusScaleCallback),
		m_cbDoF.RadiusScale,				// val	
		0.25f,								// min
		2.0f,								// max
		1e-1f);								// step
	App::AddParam(radiusScale);

	ParamVariant minLumToFilter;
	minLumToFilter.InitFloat("Renderer", "DoF", "Min Lum To Filter",
		fastdelegate::MakeDelegate(this, &DisplayPass::MinLumToFilterCallback),
		m_cbDoF.MinLumToFilter,				// val	
		1e-3f,								// min
		2.0f,								// max
		1e-3f);								// step
	App::AddParam(minLumToFilter);

	App::Filesystem::Path p(App::GetAssetDir());
	p.Append("LUT\\tony_mc_mapface.dds");
	auto err = GpuMemory::GetTexture3DFromDisk(p, m_lut);
	Check(err == LOAD_DDS_RESULT::SUCCESS, "Error while loading DDS texture from path %s: %d", p.Get(), err);

	m_descTable = renderer.GetGpuDescriptorHeap().Allocate(DESC_TABLE::COUNT);
	Direct3DUtil::CreateTexture3DSRV(m_lut, m_descTable.CPUHandle(DESC_TABLE::TONEMAPPER_LUT_SRV));

	SetDoFEnablement(dof);
}

void DisplayPass::Reset()
{
	if (IsInitialized())
	{
		m_dofGather.Reset();
		m_dofFiltered.Reset();
		RenderPassBase::ResetRenderPass();
	}
}

void DisplayPass::OnWindowResized()
{
	if (m_dof)
		CreateDoFResources();
}

void DisplayPass::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	Assert(m_compositedSrvDescHeapIdx != uint32_t(-1), "Gpu Desc Idx hasn't been set.");
	Assert(m_cbLocal.ExposureDescHeapIdx > 0, "Gpu Desc Idx hasn't been set.");

	auto& renderer = App::GetRenderer();
	auto& gpuTimer = renderer.GetGpuTimer();
	const uint32_t w = renderer.GetDisplayWidth();
	const uint32_t h = renderer.GetDisplayHeight();

	if (m_dof)
	{
		ComputeCmdList& computeCmdList = static_cast<GraphicsCmdList&>(cmdList);
		computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

		// DoF - CoC
		{
			computeCmdList.PIXBeginEvent("DoF_CoC");

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DoF_CoC");

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, DOF_CoC_THREAD_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, DOF_CoC_THREAD_GROUP_DIM_Y);

			computeCmdList.SetPipelineState(m_psosCS[(int)CS_SHADERS::DoF_CoC]);

			D3D12_TEXTURE_BARRIER barrier = Direct3DUtil::TextureBarrier(m_dofCoC.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

			computeCmdList.ResourceBarrier(barrier);

			m_cbDoF.CompositedSrvDescHeapIdx = m_compositedSrvDescHeapIdx;
			m_cbDoF.CoCSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_CoC_SRV);
			m_cbDoF.CoCUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_CoC_UAV);
			m_cbDoF.OutputUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_GATHER_UAV);

			m_rootSig.SetRootConstants(0, sizeof(m_cbDoF) / sizeof(DWORD), &m_cbDoF);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			computeCmdList.PIXEndEvent();
		}

		// DoF - Gather
		{
			computeCmdList.PIXBeginEvent("DoF_Gather");

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DoF_Gather");

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, DOF_GATHER_THREAD_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, DOF_GATHER_THREAD_GROUP_DIM_Y);

			D3D12_TEXTURE_BARRIER barriers[2];

			// transition denoiser caches into read state
			barriers[0] = Direct3DUtil::TextureBarrier(m_dofCoC.Resource(),
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
			barriers[1] = Direct3DUtil::TextureBarrier(m_dofGather.Resource(),
				D3D12_BARRIER_SYNC_NONE,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_NO_ACCESS,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

			computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));
			computeCmdList.SetPipelineState(m_psosCS[(int)CS_SHADERS::DoF_GATHER]);

			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			computeCmdList.PIXEndEvent();
		}

		// gaussian filter
#if GAUSSIAN_FILTER == 1
		{
			computeCmdList.PIXBeginEvent("DoF_GaussianFilter");

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DoF_GaussianFilter");

			const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, GAUSSIAN_FILTER_THREAD_GROUP_DIM_X);
			const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, GAUSSIAN_FILTER_THREAD_GROUP_DIM_Y);

			computeCmdList.SetPipelineState(m_psosCS[(int)CS_SHADERS::DoF_GAUSSIAN_FILTER]);

			D3D12_TEXTURE_BARRIER barriers[2];

			{
				barriers[0] = Direct3DUtil::TextureBarrier(m_dofGather.Resource(),
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
					D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
					D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
				barriers[1] = Direct3DUtil::TextureBarrier(m_dofFiltered.Resource(),
					D3D12_BARRIER_SYNC_NONE,
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
					D3D12_BARRIER_ACCESS_NO_ACCESS,
					D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

				computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

				m_cbGaussian.GatherSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_GATHER_SRV);
				m_cbGaussian.FilteredUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_FILTERED_UAV);

				m_rootSig.SetRootConstants(0, sizeof(m_cbGaussian) / sizeof(DWORD), &m_cbGaussian);
				m_rootSig.End(computeCmdList);

				computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
			}

			{
				barriers[0] = Direct3DUtil::TextureBarrier(m_dofFiltered.Resource(),
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
					D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
					D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
				barriers[1] = Direct3DUtil::TextureBarrier(m_dofGather.Resource(),
					D3D12_BARRIER_SYNC_NONE,
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
					D3D12_BARRIER_ACCESS_NO_ACCESS,
					D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

				computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

				m_cbGaussian.GatherSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_FILTERED_SRV);
				m_cbGaussian.FilteredUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_GATHER_UAV);

				m_rootSig.SetRootConstants(0, sizeof(m_cbGaussian) / sizeof(DWORD), &m_cbGaussian);
				m_rootSig.End(computeCmdList);

				computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
			}

			{
				barriers[0] = Direct3DUtil::TextureBarrier(m_dofGather.Resource(),
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
					D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
					D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
				barriers[1] = Direct3DUtil::TextureBarrier(m_dofFiltered.Resource(),
					D3D12_BARRIER_SYNC_NONE,
					D3D12_BARRIER_SYNC_COMPUTE_SHADING,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
					D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
					D3D12_BARRIER_ACCESS_NO_ACCESS,
					D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);

				computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

				m_cbGaussian.GatherSrvDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_FILTERED_SRV);
				m_cbGaussian.FilteredUavDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(DESC_TABLE::DoF_GATHER_UAV);

				m_rootSig.SetRootConstants(0, sizeof(m_cbGaussian) / sizeof(DWORD), &m_cbGaussian);
				m_rootSig.End(computeCmdList);

				computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);
			}

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			computeCmdList.PIXEndEvent();
		}
#endif
	}

	{
		directCmdList.PIXBeginEvent("Display");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "Display");

		directCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());
		directCmdList.SetPipelineState(m_psosPS[(int)PS_SHADERS::DISPLAY]);

		if (m_dof)
		{
#if GAUSSIAN_FILTER == 1
			ID3D12Resource* input = m_dofFiltered.Resource();
#else
			ID3D12Resource* input = m_dofGather.Resource();
#endif

			auto barrier = Direct3DUtil::TextureBarrier(input,
				D3D12_BARRIER_SYNC_COMPUTE_SHADING,
				D3D12_BARRIER_SYNC_PIXEL_SHADING,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS,
				D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
				D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
				D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

			directCmdList.ResourceBarrier(barrier);
		}

#if GAUSSIAN_FILTER == 1
		const auto dofSrv = DESC_TABLE::DoF_FILTERED_SRV;
#else
		const auto dofSrv = DESC_TABLE::DoF_GATHER_SRV;
#endif

		m_cbLocal.LUTDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::TONEMAPPER_LUT_SRV);
		m_cbLocal.InputDescHeapIdx = m_dof ? m_descTable.GPUDesciptorHeapIndex((int)dofSrv) : m_compositedSrvDescHeapIdx;
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

	for (int i = 0; i < (int)CS_SHADERS::COUNT; i++)
	{
#if GAUSSIAN_FILTER == 0
		if (i == (int)CS_SHADERS::DoF_GAUSSIAN_FILTER)
			continue;
#endif

		m_psosCS[i] = m_psoLib.GetComputePSO((int)PS_SHADERS::COUNT + i,
			m_rootSigObj.Get(),
			COMPILED_CS[i]);
	}
}

void DisplayPass::CreateDoFResources()
{
	auto& renderer = App::GetRenderer();

	m_descTable = renderer.GetGpuDescriptorHeap().Allocate(DESC_TABLE::COUNT);
	Direct3DUtil::CreateTexture3DSRV(m_lut, m_descTable.CPUHandle(DESC_TABLE::TONEMAPPER_LUT_SRV));

	m_dofCoC = GpuMemory::GetTexture2D("DoF_CoC",
		renderer.GetDisplayWidth(), renderer.GetDisplayHeight(),
		DXGI_FORMAT_R16_FLOAT,
		D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
		CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	m_dofGather = GpuMemory::GetTexture2D("DoF_Gather",
		renderer.GetDisplayWidth(), renderer.GetDisplayHeight(),
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
		CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	Direct3DUtil::CreateTexture2DSRV(m_dofCoC, m_descTable.CPUHandle(DESC_TABLE::DoF_CoC_SRV));
	Direct3DUtil::CreateTexture2DUAV(m_dofCoC, m_descTable.CPUHandle(DESC_TABLE::DoF_CoC_UAV));
	Direct3DUtil::CreateTexture2DSRV(m_dofGather, m_descTable.CPUHandle(DESC_TABLE::DoF_GATHER_SRV));
	Direct3DUtil::CreateTexture2DUAV(m_dofGather, m_descTable.CPUHandle(DESC_TABLE::DoF_GATHER_UAV));

#if GAUSSIAN_FILTER == 1
	m_dofFiltered = GpuMemory::GetTexture2D("DoF_Filtered",
		renderer.GetDisplayWidth(), renderer.GetDisplayHeight(),
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
		CREATE_TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	Direct3DUtil::CreateTexture2DUAV(m_dofFiltered, m_descTable.CPUHandle(DESC_TABLE::DoF_FILTERED_SRV));
	Direct3DUtil::CreateTexture2DUAV(m_dofFiltered, m_descTable.CPUHandle(DESC_TABLE::DoF_FILTERED_UAV));
#endif
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

void DisplayPass::SetDoFEnablement(const Support::ParamVariant& p)
{
	m_dof = p.GetBool();
	m_cbDoF.IsGaussianFilterEnabled = GAUSSIAN_FILTER;

	if (m_dof)
		CreateDoFResources();
	else
	{
		m_dofCoC.Reset();
		m_dofGather.Reset();
		m_dofFiltered.Reset();
	}
}

void DisplayPass::FocusDistCallback(const Support::ParamVariant& p)
{
	m_cbDoF.FocusDepth = p.GetFloat().m_val;
}

void DisplayPass::FStopCallback(const Support::ParamVariant& p)
{
	m_cbDoF.FStop = p.GetFloat().m_val;
}

void DisplayPass::FocalLengthCallback(const Support::ParamVariant& p)
{
	m_cbDoF.FocalLength = p.GetFloat().m_val;
}

void DisplayPass::BlurRadiusCallback(const Support::ParamVariant& p)
{
	m_cbDoF.MaxBlurRadius = p.GetFloat().m_val;
}

void DisplayPass::RadiusScaleCallback(const Support::ParamVariant& p)
{
	m_cbDoF.RadiusScale = p.GetFloat().m_val;
}

void DisplayPass::MinLumToFilterCallback(const Support::ParamVariant& p)
{
	m_cbDoF.MinLumToFilter = p.GetFloat().m_val;
}