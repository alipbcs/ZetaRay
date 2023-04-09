#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "Compositing_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::Support
{
	struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
	struct Compositing final
	{
		enum class SHADER_IN_GPU_DESC
		{
			DIFFUSE_DNSR_CACHE,
			SPECULAR_DNSR_CACHE,
			INSCATTERING,
			SUN_SHADOW,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			COMPOSITED,
			DoF_GATHER,
			DoF_FILTERED,
			COUNT
		};

		Compositing() noexcept;
		~Compositing() noexcept;

		void Init(bool dof = false) noexcept;
		bool IsInitialized() noexcept { return m_psos[0] != nullptr; }
		void Reset() noexcept;
		void SetInscatteringEnablement(bool b) { m_cbComposit.AccumulateInscattering = b; }
		void SetDoFEnablement(bool b) noexcept;
		void SetVoxelGridDepth(float zNear, float zFar) noexcept { m_cbComposit.VoxelGridNearZ = zNear, m_cbComposit.VoxelGridFarZ = zFar; }
		void SetVoxelGridMappingExp(float p) noexcept { m_cbComposit.DepthMappingExp = p; }
		void SetRoughnessCutoff(float c) noexcept { m_cbComposit.RoughnessCutoff = c; }
		void SetGpuDescriptor(SHADER_IN_GPU_DESC i, uint32_t descHeapIdx) noexcept
		{
			Assert((int)i < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");

			switch (i)
			{
			case SHADER_IN_GPU_DESC::DIFFUSE_DNSR_CACHE:
				m_cbComposit.DiffuseDNSRCacheDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::INSCATTERING:
				m_cbComposit.InscatteringDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::SUN_SHADOW:
				m_cbComposit.SunShadowDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::SPECULAR_DNSR_CACHE:
				m_cbComposit.SpecularDNSRCacheDescHeapIdx = descHeapIdx;
				return;
			default:
				Assert(false, "unreachable case.");
				return;
			}
		}
		const Core::Texture& GetOutput(SHADER_OUT_RES i) const noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");

			if (i == SHADER_OUT_RES::DoF_GATHER)
				return m_dofGather;

			if (i == SHADER_OUT_RES::DoF_FILTERED && (m_numGaussianPasses & 0x1) == 0)
				return m_dofGather;

			return m_hdrLightAccum;
		}
		void OnWindowResized() noexcept;
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbCompositing) / sizeof(DWORD);

		RpObjects s_rpObjs;
		Core::RootSignature m_rootSig;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT LIGHT_ACCUM = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			LIGHT_ACCUM_SRV,
			LIGHT_ACCUM_UAV,
			DoF_GATHER_SRV,
			DoF_GATHER_UAV,
			COUNT
		};

		enum class SHADERS
		{
			COMPOSIT,
			DoF_GATHER,
			DoF_GAUSSIAN_FILTER,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"Compositing_cs.cso",
			"DoF_Gather_cs.cso",
			"DoF_GaussianFilter_cs.cso"
		};
		
		Core::Texture m_hdrLightAccum;
		Core::Texture m_dofGather;
		Core::DescriptorTable m_descTable; 
		cbCompositing m_cbComposit;
		cbDoF m_cbDoF;
		cbGaussianFilter m_cbGaussian;
		int m_numGaussianPasses = 2;
		bool m_dof = false;

		void CreateLightAccumTex() noexcept;
		void CreateDoFResources() noexcept;
		void SetDirectLightingEnablementCallback(const Support::ParamVariant& p) noexcept;
		void SetIndirectDiffuseingEnablementCallback(const Support::ParamVariant& p) noexcept;
		void SetIndirectSpecularingEnablementCallback(const Support::ParamVariant& p) noexcept;
		void FocusDistCallback(const Support::ParamVariant& p) noexcept;
		void FStopCallback(const Support::ParamVariant& p) noexcept;
		void FocalLengthCallback(const Support::ParamVariant& p) noexcept;
		void BlurRadiusCallback(const Support::ParamVariant& p) noexcept;
		void RadiusScaleCallback(const Support::ParamVariant& p) noexcept;
		void MinLumToFilterCallback(const Support::ParamVariant& p) noexcept;
		void NumGaussianPassesCallback(const Support::ParamVariant& p) noexcept;

		void ReloadCompsiting() noexcept;
		void ReloadDoF() noexcept;
	};
}