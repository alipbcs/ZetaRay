#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "AutoExposure_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct AutoExposure final
	{
		enum class SHADER_IN_DESC
		{
			COMPOSITED,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			EXPOSURE,
			COUNT
		};

		AutoExposure() noexcept;
		~AutoExposure() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		void SetDescriptor(SHADER_IN_DESC i, uint32_t heapIdx) noexcept
		{
			Assert((uint32_t)i < (uint32_t)SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_inputDesc[(uint32_t)i] = heapIdx;
		}
		Core::Texture& GetOutput(SHADER_OUT_RES i) noexcept
		{
			Assert((uint32_t)i < (uint32_t)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_exposure;
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void CreateResources() noexcept;

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 1;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbAutoExposure) / sizeof(DWORD);

		RpObjects s_rpObjs;

		uint32_t m_mip5Width;
		uint32_t m_mip5Height;
		Core::Texture m_downsampledLogLumMip5;
		Core::Texture m_exposure;
		Core::DefaultHeapBuffer m_counter;
		uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT] = { 0 };

		enum class DESC_TABLE
		{
			MIP5_UAV,
			LAST_MIP_UAV,
			COUNT
		};

		Core::DescriptorTable m_descTable;

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;
		inline static constexpr const char* COMPILED_CS = "AutoExposure_cs.cso";

		struct DefaultParamVals
		{
			static constexpr float MinLum = 1e-3f;
			static constexpr float MaxLum = 16.0f;
			static constexpr bool ClampLum = true;
		};

		float m_minLum;
		float m_maxLum;
		bool m_clampLum;

		void ChangeMinLumCallback(const Support::ParamVariant& p) noexcept;
		void ChangeMaxLumCallback(const Support::ParamVariant& p) noexcept;
		void ToggleLumClampingCallback(const Support::ParamVariant& p) noexcept;
		void ReloadShader() noexcept;
	};
}