#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../SupportSystem/Param.h"
#include "FinalPass_Common.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
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
				INDIRECT_DIFFUSE_LI,
				SVGF_SPATIAL_VAR,
				SVGF_TEMPORAL_CACHE,
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
			void Render(CommandList& cmdList) noexcept;

		private:
			static constexpr int NUM_CBV = 1;
			static constexpr int NUM_SRV = 1;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 1;
			static constexpr int NUM_CONSTS = sizeof(cbFinalPass) / sizeof(DWORD);

			inline static RpObjects s_rpObjs;

			inline static const char* COMPILED_VS[] = { "FinalPass_vs.cso" };
			inline static const char* COMPILED_PS[] = { "FinalPass_ps.cso" };

			RootSignature m_rootSig;
			ID3D12PipelineState* m_pso = nullptr;

			D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };
			uint32_t m_gpuDescs[(int)SHADER_IN_GPU_DESC::COUNT] = { 0 };
			D3D12_GPU_VIRTUAL_ADDRESS m_buffers[(int)SHADER_IN_BUFFER_DESC::COUNT] = { 0 };

			struct DefaultParamVals
			{
				static constexpr float KeyValue = 0.1150f;

				enum Options
				{
					DEFAULT,
					BASE_COLOR,
					NORMALS,
					METALNESS_ROUGHNESS,
					MOTION_VECTOR,
					DEPTH,
					INDIRECT_DIFFUSE,
					SVGF_SPATIAL_VAR,
					SVGF_TEMPORAL_CACHE
				};

				inline static const char* RenderOptions[] = { "Default", "BaseColor", "Normals",
					"MetalnessRoughness", "MotionVector", "Depth", "IndirectDiffuse",
					"SVGF_SpatialVariance", "SVGF_TemporalCache" };
			};

			cbFinalPass m_cbLocal;

			// needed for shader hot-reload
			D3D12_GRAPHICS_PIPELINE_STATE_DESC m_cachedPsoDesc;

			// parameter callbacks
			void DoTonemappingCallback(const ParamVariant& p) noexcept;
			void ChangeRenderOptionCallback(const ParamVariant& p) noexcept;
			void KeyValueCallback(const ParamVariant& p) noexcept;

			// shader hot-reload
			void ReloadShaders() noexcept;
		};
	}
}