#pragma once

#include "../RenderPass.h"
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
	struct DisplayPass final : public RenderPassBase
	{
		enum class SHADER_IN_CPU_DESC
		{
			RTV,
			COUNT
		};

		enum class SHADER_IN_GPU_DESC
		{
			COMPOSITED,
			EXPOSURE,
			COUNT
		};

		DisplayPass();
		~DisplayPass();

		void Init();
		bool IsInitialized() { return m_psosPS[0] != nullptr; }
		void Reset();
		void SetCpuDescriptor(SHADER_IN_CPU_DESC i, D3D12_CPU_DESCRIPTOR_HANDLE h)
		{
			Assert((int)i < (int)SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
			m_cpuDescs[(int)i] = h;
		}
		void SetGpuDescriptor(SHADER_IN_GPU_DESC i, uint32_t dechHeapIdx)
		{
			Assert((int)i < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");
			switch (i)
			{
			case SHADER_IN_GPU_DESC::COMPOSITED:
				m_compositedSrvDescHeapIdx = dechHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::EXPOSURE:
				m_cbLocal.ExposureDescHeapIdx = dechHeapIdx;
				break;
			default:
				break;
			}
		}
		void OnWindowResized();
		void Render(Core::CommandList& cmdList);

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbDisplayPass) / sizeof(DWORD), sizeof(cbDoF_Gather) / sizeof(DWORD));

		enum class PS_SHADERS
		{
			DISPLAY,
			COUNT
		};

		enum class CS_SHADERS
		{
			DoF_CoC,
			DoF_GATHER,
			DoF_GAUSSIAN_FILTER,
			COUNT
		};

		enum DESC_TABLE
		{
			TONEMAPPER_LUT_SRV,
			DoF_CoC_SRV,
			DoF_CoC_UAV,
			DoF_GATHER_SRV,
			DoF_GATHER_UAV,
			DoF_FILTERED_SRV,
			DoF_FILTERED_UAV,
			COUNT
		};

		inline static constexpr const char* COMPILED_VS[(int)PS_SHADERS::COUNT] = { "Display_vs.cso" };
		inline static constexpr const char* COMPILED_PS[(int)PS_SHADERS::COUNT] = { "Display_ps.cso" };
		inline static constexpr const char* COMPILED_CS[(int)CS_SHADERS::COUNT] = {
			"DoF_CoC_cs.cso",
			"DoF_Gather_cs.cso",
			"DoF_GaussianFilter_cs.cso",
		};

		ID3D12PipelineState* m_psosPS[(int)PS_SHADERS::COUNT] = { 0 };
		ID3D12PipelineState* m_psosCS[(int)CS_SHADERS::COUNT] = { 0 };
		Core::GpuMemory::Texture m_lut;
		Core::GpuMemory::Texture m_dofCoC;
		Core::GpuMemory::Texture m_dofGather;
		Core::GpuMemory::Texture m_dofFiltered;
		Core::DescriptorTable m_descTable;
		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };

		cbDisplayPass m_cbLocal;
		cbDoF_Gather m_cbDoF;
		cbGaussianFilter m_cbGaussian;

		uint32_t m_compositedSrvDescHeapIdx = uint32_t(-1);
		bool m_dof = false;

		struct Params
		{
			inline static const char* DisplayOptions[] = { "Default", "BaseColor", "Normal",
				"MetalnessRoughness", "Emissive", "Depth" };
			static_assert((int)DisplayOption::COUNT == ZetaArrayLen(DisplayOptions), "enum <-> strings mismatch.");			

			inline static const char* Tonemappers[] = { "None", "ACES", "Neutral", "AgX (Default)", "AgX (Punchy)" };
			static_assert((int)Tonemapper::COUNT == ZetaArrayLen(Tonemappers), "enum <-> strings mismatch.");
		};

		void CreatePSOs();
		void CreateDoFResources();

		// parameter callbacks
		void DisplayOptionCallback(const Support::ParamVariant& p);
		void TonemapperCallback(const Support::ParamVariant& p);
		void SaturationCallback(const Support::ParamVariant& p);
		void AutoExposureCallback(const Support::ParamVariant& p);
		void SetDoFEnablement(const Support::ParamVariant& p);
		void FocusDistCallback(const Support::ParamVariant& p);
		void FStopCallback(const Support::ParamVariant& p);
		void FocalLengthCallback(const Support::ParamVariant& p);
		void BlurRadiusCallback(const Support::ParamVariant& p);
		void RadiusScaleCallback(const Support::ParamVariant& p);
		void MinLumToFilterCallback(const Support::ParamVariant& p);
	};
}