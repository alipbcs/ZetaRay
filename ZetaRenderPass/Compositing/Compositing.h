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
			SKY_DI_DENOISED,
			INSCATTERING,
			SUN_SHADOW,
			EMISSIVE_DI_DENOISED,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			COMPOSITED_DEFAULT,
			COMPOSITED_FILTERED,
			FINAL_OUTPUT,
			COUNT
		};

		Compositing() noexcept;
		~Compositing() noexcept;

		void Init(bool dof, bool skyIllum, bool fireflyFilter) noexcept;
		bool IsInitialized() noexcept { return m_psos[0] != nullptr; }
		void Reset() noexcept;
		void SetInscatteringEnablement(bool b) { m_cbComposit.AccumulateInscattering = b; }
		void SetDoFEnablement(bool b) noexcept;
		void SetSkyIllumEnablement(bool b) noexcept;
		void SetFireflyFilterEnablement(bool b) noexcept;
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
			case SHADER_IN_GPU_DESC::SKY_DI_DENOISED:
				m_cbComposit.SkyDIDenoisedDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::EMISSIVE_DI_DENOISED:
				m_cbComposit.EmissiveDIDenoisedDescHeapIdx = descHeapIdx;
				return;
			default:
				Assert(false, "unreachable case.");
				return;
			}
		}
		const Core::Texture& GetOutput(SHADER_OUT_RES i) const noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			if (i == SHADER_OUT_RES::COMPOSITED_DEFAULT)
				return m_hdrLightAccum;
			if (i == SHADER_OUT_RES::COMPOSITED_FILTERED)
				return m_dofGather_filtered;

			return *m_output;
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
			DoF_GATHER_FILTERED_SRV,
			DoF_GATHER_FILTERED_UAV,
			COUNT
		};

		enum class SHADERS
		{
			COMPOSIT,
			DoF_GATHER,
			DoF_GAUSSIAN_FILTER,
			FIREFLY_FILTER,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"Compositing_cs.cso",
			"DoF_Gather_cs.cso",
			"DoF_GaussianFilter_cs.cso",
			"FireflyFilter_cs.cso"
		};
		
		Core::Texture m_hdrLightAccum;
		Core::Texture m_dofGather_filtered;
		Core::DescriptorTable m_descTable; 
		cbCompositing m_cbComposit;
		cbDoF m_cbDoF;
		cbGaussianFilter m_cbGaussian;
		int m_numGaussianPasses = 0;
		Core::Texture* m_output = nullptr;
		bool m_filterFirefly = false;
		bool m_dof = false;
		bool m_needToUavBarrierOnHDR = false;
		bool m_needToUavBarrierOnFilter = false;

		void CreateLightAccumTex() noexcept;
		void CreateDoForFilteredResources() noexcept;
		void UpdateManualBarrierConditions() noexcept;
		
		void SetSunLightingEnablementCallback(const Support::ParamVariant& p) noexcept;
		void SetDiffuseIndirectEnablementCallback(const Support::ParamVariant& p) noexcept;
		void SetSpecularIndirectEnablementCallback(const Support::ParamVariant& p) noexcept;
		void SetEmissiveEnablementCallback(const Support::ParamVariant& p) noexcept;
		void FocusDistCallback(const Support::ParamVariant& p) noexcept;
		void FStopCallback(const Support::ParamVariant& p) noexcept;
		void FocalLengthCallback(const Support::ParamVariant& p) noexcept;
		void BlurRadiusCallback(const Support::ParamVariant& p) noexcept;
		void RadiusScaleCallback(const Support::ParamVariant& p) noexcept;
		void MinLumToFilterCallback(const Support::ParamVariant& p) noexcept;
		void NumGaussianPassesCallback(const Support::ParamVariant& p) noexcept;

		void ReloadCompsiting() noexcept;
	};
}