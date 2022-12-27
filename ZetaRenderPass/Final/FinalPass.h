#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include "FinalPass_Common.h"

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
	struct FinalPass final
	{
		enum class SHADER_IN_CPU_DESC
		{
			RTV,
			COUNT
		};

		enum class SHADER_IN_GPU_DESC
		{
			FINAL_LIGHTING,
			EXPOSURE,
			DENOISER_TEMPORAL_CACHE,
			ReSTIR_GI_TEMPORAL_RESERVOIR_A,
			ReSTIR_GI_TEMPORAL_RESERVOIR_B,
			ReSTIR_GI_TEMPORAL_RESERVOIR_C,
			ReSTIR_GI_SPATIAL_RESERVOIR_A,
			ReSTIR_GI_SPATIAL_RESERVOIR_B,
			ReSTIR_GI_SPATIAL_RESERVOIR_C,
			COUNT
		};

		FinalPass() noexcept;
		~FinalPass() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; }
		void Reset() noexcept;
		void SetCpuDescriptor(SHADER_IN_CPU_DESC i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
		{
			Assert((int)i < (int)SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
			m_cpuDescs[(int)i] = h;
		}
		void SetGpuDescriptor(SHADER_IN_GPU_DESC i, uint32_t dechHeapIdx) noexcept
		{
			Assert((int)i < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");
			m_gpuDescs[(int)i] = dechHeapIdx;
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbFinalPass) / sizeof(DWORD);

		RpObjects s_rpObjs;

		inline static constexpr const char* COMPILED_VS[] = { "FinalPass_vs.cso" };
		inline static constexpr const char* COMPILED_PS[] = { "FinalPass_ps.cso" };

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;

		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };
		uint32_t m_gpuDescs[(int)SHADER_IN_GPU_DESC::COUNT] = { 0 };

		cbFinalPass m_cbLocal;

		struct Params
		{
			inline static const char* DisplayOptions[] = { "Default", "BaseColor", "Normal",
				"MetalnessRoughness", "Depth", "STAD_TemporalCache", "ReSTIR_GI_TemporalReservoir", 
				"ReSTIR_GI_SpatialReservoir"};
			static_assert((int)DisplayOption::COUNT == ZetaArrayLen(DisplayOptions), "enum <-> strings mismatch.");			

			inline static const char* Tonemappers[] = { "None", "ACESFilmic", "UE4Filmic", "Neutral" };
			static_assert((int)Tonemapper::COUNT == ZetaArrayLen(Tonemappers), "enum <-> strings mismatch.");
		};

		void CreatePSO() noexcept;

		// parameter callbacks
		void VisualizeOcclusionCallback(const Support::ParamVariant& p) noexcept;
		void ChangeDisplayOptionCallback(const Support::ParamVariant& p) noexcept;
		void ChangeTonemapperCallback(const Support::ParamVariant& p) noexcept;

		void ReloadShaders() noexcept;
	};
}