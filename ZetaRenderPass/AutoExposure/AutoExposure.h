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
		enum class MODE
		{
			DOWNSAMPLE,
			HISTOGRAM
		};

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

		void Init(MODE m = MODE::HISTOGRAM) noexcept;
		bool IsInitialized() noexcept { return m_psos[0] != nullptr || m_psos[1] != nullptr; };
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
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 1;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbAutoExposureDownsample) / sizeof(DWORD),
			sizeof(cbAutoExposureHist) / sizeof(DWORD));

		void CreateDownsampleResources() noexcept;
		void CreateHistogramResources() noexcept;
		void SwitchMode(MODE m) noexcept;

		RpObjects s_rpObjs;

		uint32_t m_mip5Width;
		uint32_t m_mip5Height;
		Core::Texture m_downsampledLogLumMip5;
		Core::Texture m_exposure;
		Core::DefaultHeapBuffer m_counter;
		Core::DefaultHeapBuffer m_hist;
		Core::DefaultHeapBuffer m_zeroBuffer;		// for resetting the histogram to zero each frame
		uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT] = { 0 };

		enum class DESC_TABLE
		{
			MIP5_UAV,
			EXPOSURE_UAV,
			HISTOGRAM_UAV,
			COUNT
		};

		Core::DescriptorTable m_descTable;

		enum class SHADERS
		{
			DOWNSAMPLE,
			HISTOGRAM,
			EXPECTED_VALUE,
			COUNT
		};

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };
		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] =
		{
			"AutoExposure_Downsample_cs.cso",
			"AutoExposure_Histogram_cs.cso",
			"AutoExposure_ExpectedVal_cs.cso"
		};

		struct DefaultParamVals
		{
			static constexpr float MinLum = 5e-3f;
			static constexpr float MaxLum = 4.0f;
			static constexpr bool ClampLum = true;
			static constexpr float EPS = 1e-4f;
			static constexpr float LumMappingExp = 0.5f;
			static constexpr float AdaptationRate = 0.3f;
			static constexpr float LowerPercentile = 0.01f;
			static constexpr float UpperPercentile = 0.9f;
		};

		MODE m_mode;
		float m_eps;
		float m_minLum;
		float m_maxLum;
		float m_lumExp;
		float m_adaptationRate;
		float m_lowerPercentile;
		float m_upperPercentile;
		bool m_clampLum;

		void MinLumCallback(const Support::ParamVariant& p) noexcept;
		void MaxLumCallback(const Support::ParamVariant& p) noexcept;
		void LumClampingCallback(const Support::ParamVariant& p) noexcept;
		void LumMappingExpCallback(const Support::ParamVariant& p) noexcept;
		void AdaptationRateCallback(const Support::ParamVariant& p) noexcept;
		void LowerPercentileCallback(const Support::ParamVariant& p) noexcept;
		void UpperPercentileCallback(const Support::ParamVariant& p) noexcept;
	};
}