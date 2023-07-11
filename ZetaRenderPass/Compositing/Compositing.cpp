#include "Compositing.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Scene/SceneRenderer.h>
#include <Support/Param.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;

//--------------------------------------------------------------------------------------
// Compositing
//--------------------------------------------------------------------------------------

Compositing::Compositing() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
	// root constants
	m_rootSig.InitAsConstants(0,				// root idx
		sizeof(cbCompositing) / sizeof(DWORD),	// num DWORDs
		0,										// register
		0);										// register space

	// frame constants
	m_rootSig.InitAsCBV(1,												// root idx
		1,																// register
		0,																// register space
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,	// flags
		D3D12_SHADER_VISIBILITY_ALL,									// visibility
		GlobalResource::FRAME_CONSTANTS_BUFFER_NAME);
}

Compositing::~Compositing() noexcept
{
	Reset();
}

void Compositing::Init(bool dof, bool skyIllum, bool fireflyFilter) noexcept
{
	auto& renderer = App::GetRenderer();
	auto samplers = renderer.GetStaticSamplers();

	D3D12_ROOT_SIGNATURE_FLAGS flags =
		D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	s_rpObjs.Init("Compositing", m_rootSig, samplers.size(), samplers.data(), flags);

	for (int i = 0; i < (int)SHADERS::COUNT; i++)
	{
		m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i,
			s_rpObjs.m_rootSig.Get(),
			COMPILED_CS[i]);
	}

	m_descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate((int)DESC_TABLE::COUNT);
	CreateLightAccumTex();

	m_cbComposit.AccumulateInscattering = false;
	m_cbComposit.SunLighting = true;
	m_cbComposit.SkyLighting = skyIllum;
	m_cbComposit.DiffuseIndirect = true;
	m_cbComposit.SpecularIndirect = true;
	m_cbComposit.RoughnessCutoff = 1.0f;
	m_cbComposit.FocusDepth = 5.0f;
	m_cbComposit.FocalLength = 50;
	m_cbComposit.FStop = 1.4f;
	m_cbDoF.MaxBlurRadius = 16.0f;
	m_cbDoF.RadiusScale = 1.6f;
	m_cbDoF.MinLumToFilter = 1.0f;

	ParamVariant p0;
	p0.InitBool("Renderer", "Lighting", "SunLighting", fastdelegate::MakeDelegate(this, &Compositing::SetSunLightingEnablementCallback),
		m_cbComposit.SunLighting);
	App::AddParam(p0);	

	ParamVariant p2;
	p2.InitBool("Renderer", "Lighting", "DiffuseIndirect", fastdelegate::MakeDelegate(this, &Compositing::SetDiffuseIndirectEnablementCallback),
		m_cbComposit.DiffuseIndirect);
	App::AddParam(p2);

	ParamVariant p6;
	p6.InitBool("Renderer", "Lighting", "SpecularIndirect", fastdelegate::MakeDelegate(this, &Compositing::SetSpecularIndirectEnablementCallback),
		m_cbComposit.SpecularIndirect);
	App::AddParam(p6);

	ParamVariant focusDist;
	focusDist.InitFloat("Renderer", "DoF", "Focus Distance",
		fastdelegate::MakeDelegate(this, &Compositing::FocusDistCallback),
		m_cbComposit.FocusDepth,			// val	
		0.1f,								// min
		25.0f,								// max
		1e-3f);								// step
	App::AddParam(focusDist);

	ParamVariant fstop;
	fstop.InitFloat("Renderer", "DoF", "F-Stop",
		fastdelegate::MakeDelegate(this, &Compositing::FStopCallback),
		m_cbComposit.FStop,					// val	
		1e-1f,								// min
		5.0f,								// max
		1e-2f);								// step
	App::AddParam(fstop);

	ParamVariant focalLen;
	focalLen.InitFloat("Renderer", "DoF", "Focal Length (mm)",
		fastdelegate::MakeDelegate(this, &Compositing::FocalLengthCallback),
		m_cbComposit.FocalLength,			// val	
		1.0f,								// min
		100.0f,								// max
		1e-1f);								// step
	App::AddParam(focalLen);

	ParamVariant blurRadius;
	blurRadius.InitFloat("Renderer", "DoF", "MaxBlurRadius",
		fastdelegate::MakeDelegate(this, &Compositing::BlurRadiusCallback),
		m_cbDoF.MaxBlurRadius,				// val	
		5.0f,								// min
		25.0f,								// max
		1e-1f);								// step
	App::AddParam(blurRadius);

	ParamVariant radiusScale;
	radiusScale.InitFloat("Renderer", "DoF", "RadiusScale",
		fastdelegate::MakeDelegate(this, &Compositing::RadiusScaleCallback),
		m_cbDoF.RadiusScale,				// val	
		0.25f,								// min
		2.0f,								// max
		1e-1f);								// step
	App::AddParam(radiusScale);	
	
	ParamVariant gaussianPasses;
	gaussianPasses.InitInt("Renderer", "DoF", "#GaussianFilterPasses",
		fastdelegate::MakeDelegate(this, &Compositing::NumGaussianPassesCallback),
		m_numGaussianPasses,				// val	
		0,									// min
		3,									// max
		1);									// step
	App::AddParam(gaussianPasses);

	ParamVariant minLumToFilter;
	minLumToFilter.InitFloat("Renderer", "DoF", "MinLumToFilter",
		fastdelegate::MakeDelegate(this, &Compositing::MinLumToFilterCallback),
		m_cbDoF.MinLumToFilter,				// val	
		1e-3f,								// min
		2.0f,								// max
		1e-3f);								// step
	App::AddParam(minLumToFilter);

	App::AddShaderReloadHandler("Compositing", fastdelegate::MakeDelegate(this, &Compositing::ReloadCompsiting));

	SetDoFEnablement(dof);
	SetFireflyFilterEnablement(fireflyFilter);
}

void Compositing::Reset() noexcept
{
	if (IsInitialized())
	{
		m_hdrLightAccum.Reset();
		m_dofGather_filtered.Reset();
		s_rpObjs.Clear();
	}
}

void Compositing::OnWindowResized() noexcept
{
	CreateLightAccumTex();

	if (m_dof || m_filterFirefly)
		CreateDoForFilteredResources();

	UpdateManualBarrierConditions();
}

void Compositing::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);
	
	const int w = App::GetRenderer().GetRenderWidth();
	const int h = App::GetRenderer().GetRenderHeight();
	auto& gpuTimer = App::GetRenderer().GetGpuTimer();

	computeCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());

	// compositing
	{
		computeCmdList.PIXBeginEvent("Compositing");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "Compositing");

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, COMPOSITING_THREAD_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, COMPOSITING_THREAD_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::COMPOSIT]);

		if (m_cbComposit.AccumulateInscattering)
		{
			Assert(m_cbComposit.InscatteringDescHeapIdx > 0, "Gpu descriptor for inscattering texture hasn't been set");
			Assert(m_cbComposit.VoxelGridNearZ >= 0.0f, "Invalid voxel grid depth");
			Assert(m_cbComposit.VoxelGridFarZ > m_cbComposit.VoxelGridNearZ, "Invalid voxel grid depth");
			Assert(m_cbComposit.DepthMappingExp > 0.0f, "Invalid voxel grid depth mapping exponent");
		}

		m_cbComposit.CompositedUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::LIGHT_ACCUM_UAV);
		m_rootSig.SetRootConstants(0, sizeof(cbCompositing) / sizeof(DWORD), &m_cbComposit);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	if (m_filterFirefly)
	{
		computeCmdList.PIXBeginEvent("FireflyFilter");

		// record the timestamp prior to execution
		const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "FireflyFilter");

		const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, FIREFLY_FILTER_THREAD_GROUP_DIM_X);
		const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, FIREFLY_FILTER_THREAD_GROUP_DIM_Y);

		computeCmdList.SetPipelineState(m_psos[(int)SHADERS::FIREFLY_FILTER]);

		computeCmdList.ResourceBarrier(m_hdrLightAccum.GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		cbFireflyFilter cb;
		cb.CompositedSRVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::LIGHT_ACCUM_SRV);
		cb.FilteredUAVDescHeapIdx = m_descTable.GPUDesciptorHeapIndex((int)DESC_TABLE::DoF_GATHER_FILTERED_UAV);
		cb.DoFIsON = m_dof;

		m_rootSig.SetRootConstants(0, sizeof(cbFireflyFilter) / sizeof(DWORD), &cb);
		m_rootSig.End(computeCmdList);

		computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

		// record the timestamp after execution
		gpuTimer.EndQuery(computeCmdList, queryIdx);

		computeCmdList.PIXEndEvent();
	}

	if (m_dof)
	{
		// DoF_Gather
		{
			computeCmdList.PIXBeginEvent("DoF_Gather");

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DoF_Gather");

			const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, DOF_GATHER_THREAD_GROUP_DIM_X);
			const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, DOF_GATHER_THREAD_GROUP_DIM_Y);

			D3D12_RESOURCE_BARRIER barriers[2];
			barriers[0] = Direct3DHelper::TransitionBarrier(m_filterFirefly ? m_dofGather_filtered.GetResource() : m_hdrLightAccum.GetResource(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			barriers[1] = Direct3DHelper::TransitionBarrier(m_hdrLightAccum.GetResource(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			computeCmdList.ResourceBarrier(barriers, m_filterFirefly ? 2 : 1);

			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DoF_GATHER]);

			m_cbDoF.InputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(m_filterFirefly ?
				(int)DESC_TABLE::DoF_GATHER_FILTERED_SRV :
				(int)DESC_TABLE::LIGHT_ACCUM_SRV);
			m_cbDoF.OutputDescHeapIdx = m_descTable.GPUDesciptorHeapIndex(m_filterFirefly ?
				(int)DESC_TABLE::LIGHT_ACCUM_UAV :
				(int)DESC_TABLE::DoF_GATHER_FILTERED_UAV);

			m_rootSig.SetRootConstants(0, sizeof(m_cbDoF) / sizeof(DWORD), &m_cbDoF);
			m_rootSig.End(computeCmdList);

			computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			computeCmdList.PIXEndEvent();
		}

		// gaussian filter
		{
			computeCmdList.PIXBeginEvent("DoF_GaussianFilter");

			// record the timestamp prior to execution
			const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "DoF_GaussianFilter");

			const uint32_t dispatchDimX = (uint32_t)CeilUnsignedIntDiv(w, GAUSSIAN_FILTER_THREAD_GROUP_DIM_X);
			const uint32_t dispatchDimY = (uint32_t)CeilUnsignedIntDiv(h, GAUSSIAN_FILTER_THREAD_GROUP_DIM_Y);

			computeCmdList.SetPipelineState(m_psos[(int)SHADERS::DoF_GAUSSIAN_FILTER]);

			ID3D12Resource* src = m_filterFirefly ? m_hdrLightAccum.GetResource() : m_dofGather_filtered.GetResource();
			ID3D12Resource* target = m_filterFirefly ? m_dofGather_filtered.GetResource() : m_hdrLightAccum.GetResource();
			const auto srvEven = m_filterFirefly ? DESC_TABLE::LIGHT_ACCUM_SRV : DESC_TABLE::DoF_GATHER_FILTERED_SRV;
			const auto srvOdd = m_filterFirefly ? DESC_TABLE::DoF_GATHER_FILTERED_SRV : DESC_TABLE::LIGHT_ACCUM_SRV;
			const auto uavEven = m_filterFirefly ? DESC_TABLE::DoF_GATHER_FILTERED_UAV : DESC_TABLE::LIGHT_ACCUM_UAV;
			const auto uavOdd = m_filterFirefly ? DESC_TABLE::LIGHT_ACCUM_UAV : DESC_TABLE::DoF_GATHER_FILTERED_UAV;

			for (int i = 0; i < m_numGaussianPasses; i++)
			{
				D3D12_RESOURCE_BARRIER barriers[2];
				barriers[0] = Direct3DHelper::TransitionBarrier(src,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				barriers[1] = Direct3DHelper::TransitionBarrier(target,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				computeCmdList.ResourceBarrier(barriers, ZetaArrayLen(barriers));

				m_cbGaussian.GatherSrvDescHeapIdx = (i & 0x1) == 0 ?
					m_descTable.GPUDesciptorHeapIndex((int)srvEven) :
					m_descTable.GPUDesciptorHeapIndex((int)srvOdd);
				m_cbGaussian.FilteredUavDescHeapIdx = (i & 0x1) == 0 ?
					m_descTable.GPUDesciptorHeapIndex((int)uavEven) :
					m_descTable.GPUDesciptorHeapIndex((int)uavOdd);

				m_rootSig.SetRootConstants(0, sizeof(m_cbGaussian) / sizeof(DWORD), &m_cbGaussian);
				m_rootSig.End(computeCmdList);

				computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

				std::swap(src, target);
			}

			// record the timestamp after execution
			gpuTimer.EndQuery(computeCmdList, queryIdx);

			computeCmdList.PIXEndEvent();
		}
	}

	if (m_needToUavBarrierOnHDR)
	{
		computeCmdList.ResourceBarrier(m_hdrLightAccum.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	if (m_needToUavBarrierOnFilter)
	{
		computeCmdList.ResourceBarrier(m_dofGather_filtered.GetResource(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
}

void Compositing::CreateLightAccumTex() noexcept
{
	auto& renderer = App::GetRenderer();
	auto& gpuMem = renderer.GetGpuMemory();
	const int width = renderer.GetRenderWidth();
	const int height = renderer.GetRenderHeight();

	D3D12_CLEAR_VALUE clearValue = {};
	memset(clearValue.Color, 0, sizeof(float) * 4);
	clearValue.Format = ResourceFormats::LIGHT_ACCUM;

	m_hdrLightAccum = gpuMem.GetTexture2D("HDRLightAccum",
		width, height,
		ResourceFormats::LIGHT_ACCUM,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_RENDER_TARGET | TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS, 
		1, 
		&clearValue);

	Direct3DHelper::CreateTexture2DSRV(m_hdrLightAccum, m_descTable.CPUHandle((int)DESC_TABLE::LIGHT_ACCUM_SRV));
	Direct3DHelper::CreateTexture2DUAV(m_hdrLightAccum, m_descTable.CPUHandle((int)DESC_TABLE::LIGHT_ACCUM_UAV));
}

void Compositing::CreateDoForFilteredResources() noexcept
{
	auto& renderer = App::GetRenderer();

	m_dofGather_filtered = renderer.GetGpuMemory().GetTexture2D("DoF_Gather_Filtered",
		renderer.GetRenderWidth(), renderer.GetRenderHeight(),
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		D3D12_RESOURCE_STATE_COMMON,
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	Direct3DHelper::CreateTexture2DSRV(m_dofGather_filtered, m_descTable.CPUHandle((int)DESC_TABLE::DoF_GATHER_FILTERED_SRV));
	Direct3DHelper::CreateTexture2DUAV(m_dofGather_filtered, m_descTable.CPUHandle((int)DESC_TABLE::DoF_GATHER_FILTERED_UAV));
}

void Compositing::UpdateManualBarrierConditions() noexcept
{
	m_needToUavBarrierOnHDR = m_filterFirefly && (!m_dof || (m_dof && (m_numGaussianPasses & 0x1) == 1));
	m_needToUavBarrierOnHDR = m_needToUavBarrierOnHDR || (!m_filterFirefly && m_dof && (m_numGaussianPasses & 0x1) == 0);

	m_needToUavBarrierOnFilter = m_filterFirefly && m_dof && (m_numGaussianPasses & 0x1) == 0;
	m_needToUavBarrierOnFilter = m_needToUavBarrierOnFilter || (!m_filterFirefly && m_dof && (m_numGaussianPasses & 0x1) == 1);

	m_output = &m_hdrLightAccum;

	if (!m_dof && !m_filterFirefly)
		return;

	if (m_filterFirefly)
	{
		if (!m_dof || (m_dof && (m_numGaussianPasses & 0x1) == 1))
		{
			m_output = &m_dofGather_filtered;
			return;
		}
	}
	// dof ON
	else if (m_dof && (m_numGaussianPasses & 0x1) == 0)
		m_output = &m_dofGather_filtered;
}

void Compositing::SetDoFEnablement(bool b) noexcept
{
	m_dof = b;
	m_cbDoF.IsGaussianFilterEnabled = m_numGaussianPasses >= 1;

	UpdateManualBarrierConditions();

	if (!m_filterFirefly)
	{
		if (!m_dof)
		{
			m_dofGather_filtered.Reset();
			return;
		}
		else
			CreateDoForFilteredResources();	
	}
}

void Compositing::SetSkyIllumEnablement(bool b) noexcept
{
	m_cbComposit.SkyLighting = b;
}

void Compositing::SetFireflyFilterEnablement(bool b) noexcept
{
	m_filterFirefly = b;

	UpdateManualBarrierConditions();

	if (!m_dof)
	{
		if(!m_filterFirefly)
		{
			m_dofGather_filtered.Reset();
			return;	
		}
		else
			CreateDoForFilteredResources();
	}
}

void Compositing::SetSunLightingEnablementCallback(const Support::ParamVariant& p) noexcept
{
	m_cbComposit.SunLighting = p.GetBool();
}

void Compositing::SetDiffuseIndirectEnablementCallback(const Support::ParamVariant& p) noexcept
{
	m_cbComposit.DiffuseIndirect = p.GetBool();
}

void Compositing::SetSpecularIndirectEnablementCallback(const Support::ParamVariant& p) noexcept
{
	m_cbComposit.SpecularIndirect = p.GetBool();
}

void Compositing::FocusDistCallback(const Support::ParamVariant& p) noexcept
{
	m_cbComposit.FocusDepth = p.GetFloat().m_val;
}

void Compositing::FStopCallback(const Support::ParamVariant& p) noexcept
{
	m_cbComposit.FStop = p.GetFloat().m_val;
}

void Compositing::FocalLengthCallback(const Support::ParamVariant& p) noexcept
{
	m_cbComposit.FocalLength = p.GetFloat().m_val;
}

void Compositing::BlurRadiusCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDoF.MaxBlurRadius = p.GetFloat().m_val;
}

void Compositing::RadiusScaleCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDoF.RadiusScale = p.GetFloat().m_val;
}

void Compositing::NumGaussianPassesCallback(const Support::ParamVariant& p) noexcept
{
	m_numGaussianPasses = p.GetInt().m_val;
	m_cbDoF.IsGaussianFilterEnabled = m_numGaussianPasses >= 1;
	
	UpdateManualBarrierConditions();
}

void Compositing::MinLumToFilterCallback(const Support::ParamVariant& p) noexcept
{
	m_cbDoF.MinLumToFilter = p.GetFloat().m_val;
}

void Compositing::ReloadCompsiting() noexcept
{
	const int i = (int)SHADERS::COMPOSIT;

	s_rpObjs.m_psoLib.Reload(0, "Compositing\\Compositing.hlsl", true);
	m_psos[i] = s_rpObjs.m_psoLib.GetComputePSO(i, s_rpObjs.m_rootSig.Get(), COMPILED_CS[i]);
}