#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Support/Param.h"
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

		enum class SHADER_IN_BUFFER_DESC
		{
			AVG_LUM,
			COUNT
		};

		enum class SHADER_IN_GPU_DESC
		{
			FINAL_LIGHTING,
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

		void Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc) noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; }
		void Reset() noexcept;
		void SetBuffer(SHADER_IN_BUFFER_DESC i, D3D12_GPU_VIRTUAL_ADDRESS a) noexcept
		{
			Assert((int)i < (int)SHADER_IN_BUFFER_DESC::COUNT, "out-of-bound access.");
			m_buffers[(int)i] = a;
		}
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
		static constexpr int NUM_SRV = 1;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbFinalPass) / sizeof(DWORD);

		inline static RpObjects s_rpObjs;

		inline static const char* COMPILED_VS[] = { "FinalPass_vs.cso" };
		inline static const char* COMPILED_PS[] = { "FinalPass_ps.cso" };

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;

		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };
		uint32_t m_gpuDescs[(int)SHADER_IN_GPU_DESC::COUNT] = { 0 };
		D3D12_GPU_VIRTUAL_ADDRESS m_buffers[(int)SHADER_IN_BUFFER_DESC::COUNT] = { 0 };

		struct Params
		{
			enum Options
			{
				DEFAULT,
				BASE_COLOR,
				NORMAL,
				METALNESS_ROUGHNESS,
				DEPTH,
				STAD_TEMPORAL_CACHE,
				ReSTIR_GI_TEMPORAL_RESERVOIR,
				ReSTIR_GI_SPATIAL_RESERVOIR,
				COUNT
			};

			inline static const char* RenderOptions[] = { "Default", "BaseColor", "Normal",
				"MetalnessRoughness", "Depth", "STAD_TemporalCache", "ReSTIR_GI_TemporalReservoir", 
				"ReSTIR_GI_SpatialReservoir"};

			static_assert(Options::COUNT == ZetaArrayLen(RenderOptions), "enum <-> strings mismatch.");
		};

		cbFinalPass m_cbLocal;

		// needed for shader hot-reload
		D3D12_GRAPHICS_PIPELINE_STATE_DESC m_cachedPsoDesc;

		// parameter callbacks
		void DoTonemappingCallback(const Support::ParamVariant& p) noexcept;
		void VisualizeOcclusionCallback(const Support::ParamVariant& p) noexcept;
		void ChangeRenderOptionCallback(const Support::ParamVariant& p) noexcept;

		void ReloadShaders() noexcept;
	};
}