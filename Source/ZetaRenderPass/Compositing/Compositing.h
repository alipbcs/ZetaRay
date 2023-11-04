#pragma once

#include "../RenderPass.h"
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
	struct Compositing final : public RenderPassBase
	{
		enum class SHADER_IN_GPU_DESC
		{
			SKY_DI_DENOISED,
			INSCATTERING,
			SUN_SHADOW,
			EMISSIVE_DI_DENOISED,
			INDIRECT_DENOISED,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			COMPOSITED,
			COUNT
		};

		Compositing();
		~Compositing();

		void Init(bool skyIllum);
		bool IsInitialized() { return m_psos[0] != nullptr; }
		void Reset();
		void SetInscatteringEnablement(bool enable) { SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::INSCATTERING, enable); }
		void SetSkyIllumEnablement(bool enable);
		void SetVoxelGridDepth(float zNear, float zFar) { m_cbComposit.VoxelGridNearZ = zNear, m_cbComposit.VoxelGridFarZ = zFar; }
		void SetVoxelGridMappingExp(float exp) { m_cbComposit.DepthMappingExp = exp; }
		void SetGpuDescriptor(SHADER_IN_GPU_DESC input, uint32_t descHeapIdx)
		{
			Assert((int)input < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");

			switch (input)
			{
			case SHADER_IN_GPU_DESC::INSCATTERING:
				m_cbComposit.InscatteringDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::SUN_SHADOW:
				m_cbComposit.SunShadowDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::SKY_DI_DENOISED:
				m_cbComposit.SkyDIDenoisedDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::EMISSIVE_DI_DENOISED:
				m_cbComposit.EmissiveDIDenoisedDescHeapIdx = descHeapIdx;
				return;
			case SHADER_IN_GPU_DESC::INDIRECT_DENOISED:
				m_cbComposit.IndirectDenoisedDescHeapIdx = descHeapIdx;
				return;
			default:
				Assert(false, "unreachable case.");
				return;
			}
		}
		const Core::GpuMemory::Texture & GetOutput(SHADER_OUT_RES out) const
		{
			Assert((int)out < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");
			return m_hdrLightAccum;
		}
		void OnWindowResized();
		void Render(Core::CommandList& cmdList);

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbCompositing) / sizeof(DWORD);

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT LIGHT_ACCUM = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			LIGHT_ACCUM_UAV,
			COUNT
		};

		enum class SHADERS
		{
			COMPOSIT,
			FIREFLY_FILTER,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"Compositing_cs.cso",
			"FireflyFilter_cs.cso"
		};
		
		Core::GpuMemory::Texture m_hdrLightAccum;
		Core::DescriptorTable m_descTable;
		cbCompositing m_cbComposit;
		bool m_filterFirefly = false;
		bool m_needToUavBarrierOnHDR = false;
		bool m_needToUavBarrierOnFilter = false;

		void CreateLightAccumTexure();

		void SetFireflyFilterEnablement(const Support::ParamVariant& p);
		void SetSunLightingEnablementCallback(const Support::ParamVariant& p);
		void SetIndirectEnablementCallback(const Support::ParamVariant& p);
		void SetEmissiveEnablementCallback(const Support::ParamVariant& p);

		void ReloadCompsiting();
	};
}