#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "Display_Common.h"

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
	struct DisplayPass final
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
			DIFFUSE_DNSR_CACHE,
			ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR_A,
			ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR_B,
			ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR_A,
			ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR_B,
			COUNT
		};

		DisplayPass() noexcept;
		~DisplayPass() noexcept;

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
			switch (i)
			{
			case SHADER_IN_GPU_DESC::FINAL_LIGHTING:
				m_cbLocal.InputDescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::EXPOSURE:
				m_cbLocal.ExposureDescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::DIFFUSE_DNSR_CACHE:
				m_cbLocal.DiffuseDNSRTemporalCacheDescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR_A:
				m_cbLocal.DiffuseTemporalReservoir_A_DescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_TEMPORAL_RESERVOIR_B:
				m_cbLocal.DiffuseTemporalReservoir_B_DescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR_A:
				m_cbLocal.DiffuseSpatialReservoir_A_DescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::ReSTIR_GI_DIFFUSE_SPATIAL_RESERVOIR_B:
				m_cbLocal.DiffuseSpatialReservoir_B_DescHeapIdx = dechHeapIdx;
				break;
			default:
				break;
			}
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbDisplayPass) / sizeof(DWORD);

		RpObjects s_rpObjs;

		inline static constexpr const char* COMPILED_VS[] = { "Display_vs.cso" };
		inline static constexpr const char* COMPILED_PS[] = { "Display_ps.cso" };

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;
		Core::Texture m_lut;
		Core::DescriptorTable m_lutSRV;
		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };

		cbDisplayPass m_cbLocal;

		struct Params
		{
			inline static const char* DisplayOptions[] = { "Default", "BaseColor", "Normal",
				"MetalnessRoughness", "Emissive", "Depth", "Curvature", "ExposureHeatmap", "DiffuseDNSR", "ReSTIR_GI_Diffuse_Temporal", 
				"ReSTIR_GI_Diffuse_Spatial"};
			static_assert((int)DisplayOption::COUNT == ZetaArrayLen(DisplayOptions), "enum <-> strings mismatch.");			

			inline static const char* Tonemappers[] = { "None", "ACESFitted", "Neutral" };
			static_assert((int)Tonemapper::COUNT == ZetaArrayLen(Tonemappers), "enum <-> strings mismatch.");
		};

		void CreatePSO() noexcept;

		// parameter callbacks
		void DisplayOptionCallback(const Support::ParamVariant& p) noexcept;
		void TonemapperCallback(const Support::ParamVariant& p) noexcept;
		void SaturationCallback(const Support::ParamVariant& p) noexcept;
		void AutoExposureCallback(const Support::ParamVariant& p) noexcept;
	};
}