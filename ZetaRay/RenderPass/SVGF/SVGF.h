#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "SVGF_Common.h"

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
	struct SVGF final
	{
		enum class SHADER_IN_RES
		{
			INDIRECT_LI,
			LINEAR_DEPTH_GRAD,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			TEMPORAL_CACHE_COL_LUM_A,
			TEMPORAL_CACHE_COL_LUM_B,
			SPATIAL_VAR,
			COUNT
		};

		SVGF() noexcept;
		~SVGF() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_psos[0] != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		void SetDescriptor(SHADER_IN_RES i, uint32_t heapIdx) noexcept
		{
			const int idx = (int)i;
			Assert(idx < (int)SHADER_IN_RES::COUNT, "out-of-bound access.");
			m_inputGpuHeapIndices[idx] = heapIdx;
		}
		const Core::Texture& GetOutput(SHADER_OUT_RES i) const noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");

			switch (i)
			{
			case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A:
				return m_temporalCacheColLum[0];
			case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B:
				return m_temporalCacheColLum[1];
				//case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_TSPP:
				//	return m_temporalCacheTSPP;
			default:
				if (!m_filterSpatialVariance)
					return m_spatialLumVar;
				else
					return m_spatialLumVarFiltered;
			}
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void CreateResources() noexcept;
		void InitParams() noexcept;
		void ReloadSpatialVar() noexcept;
		void ReloadTemporalPass() noexcept;
		void ReloadGaussianFilter() noexcept;
		void ReloadWaveletFilter() noexcept;

		enum class SHADERS
		{
			TEMPORAL_FILTER,
			SPATIAL_VARIANCE,
			ATROUS_WAVELET_TRANSFORM,
			GAUSSIAN_FILTER,
			COUNT
		};

		enum class DESC_TABLE
		{
			INDIRECT_LI_SRV,
			LINEAR_DEPTH_GRAD_SRV,
			TEMPORAL_CACHE_COL_LUM_A_SRV,
			TEMPORAL_CACHE_COL_LUM_A_UAV,
			TEMPORAL_CACHE_COL_LUM_B_SRV,
			TEMPORAL_CACHE_COL_LUM_B_UAV,
			SPATIAL_LUM_VAR_UAV,
			SPATIAL_LUM_VAR_FILTERED_UAV,
			TEMPORAL_CACHE_TSPP_UAV,
			COUNT
		};

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr size_t NUM_CONSTS = std::max(
			std::max(sizeof(cbAtrousWaveletFilter) / sizeof(DWORD), sizeof(cbTemporalFilter) / sizeof(DWORD)),
			std::max(sizeof(cbSpatialVar) / sizeof(DWORD), sizeof(cbGaussianFilter) / sizeof(DWORD)));

		inline static RpObjects s_rpObjs;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT TEMPORAL_CACHE_COLOR_LUM = DXGI_FORMAT_R32G32B32A32_UINT;
			static constexpr DXGI_FORMAT TEMPORAL_CACHE_TSPP = DXGI_FORMAT_R8_UINT;
			static constexpr DXGI_FORMAT SPATIAL_LUM_VAR = DXGI_FORMAT_R16_FLOAT;
		};

		struct DefaultParamVals
		{
			static constexpr int NumWaveletTransformPasses = 1;
			static constexpr int MaxTSPP = 32;
			static constexpr int MinTSPPtoUseTemporalVar = 4;
			static constexpr float BilinearNormalScale = 1.4f;
			static constexpr float BilinearNormalExp = 16.0f;
			static constexpr float BilinearGeometryMaxPlaneDist = 0.1f;
			//static constexpr bool NeighborhoodClamping = true;
			//static constexpr float NeighborhoodClampingStdScale = 0.6f;
			//static constexpr float NeighborhoodClampingMinStd = 0.05f;
			//static constexpr float NeighborhoodClampingTSPPAdj = 4.0f;
			static constexpr float MinLumVariance = 0.1f;
			static constexpr float MinConsistentWeight = 1e-3f;
			static constexpr int SpatialVarianceRadius = 4;
			static constexpr float EdgeStoppingDepthWeightCutoff = 0.2f;
			static constexpr float EdgeStoppingLumSigma = 4.0f;
			static constexpr float EdgeStoppingNormalSigma = 128.0f;
			static constexpr float EdgeStoppingDepthSigma = 1.0f;
		};

		// all the shaders use the same root signature
		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };

		uint32_t m_inputGpuHeapIndices[(int)SHADER_IN_RES::COUNT];

		Core::Texture m_temporalCacheColLum[2];
		Core::Texture m_spatialLumVar;
		Core::Texture m_spatialLumVarFiltered;

		// descriptor-table containing all the needed descriptors
		Core::DescriptorTable m_descTable;
		bool m_filterSpatialVariance = false;
		bool m_isTemporalCacheValid = false;
		int m_numWaveletFilterPasses = DefaultParamVals::NumWaveletTransformPasses;
		bool m_waveletTransform = false;

		// constant buffers
		cbTemporalFilter m_cbTemporalFilter;
		cbSpatialVar m_cbSpatialVar;
		cbAtrousWaveletFilter m_cbWaveletTransform;
		cbGaussianFilter m_cbGaussianFilter;

		// parameter callbacks
		void MaxTSPPCallback(const Support::ParamVariant& p) noexcept;
		void MinTSPPForTemporalVarCallback(const Support::ParamVariant& p) noexcept;
		void BilinearNormalScaleCallback(const Support::ParamVariant& p) noexcept;
		void BilinearNormalExpCallback(const Support::ParamVariant& p) noexcept;
		void BilinearGeometryMaxPlaneDistCallback(const Support::ParamVariant& p) noexcept;
		void MinLumVarCallback(const Support::ParamVariant& p) noexcept;
		void MinConsistentWeightCallback(const Support::ParamVariant& p) noexcept;
		void SpatialVarRadiusCallback(const Support::ParamVariant& p) noexcept;
		void FilterSpatialVarCallback(const Support::ParamVariant& p) noexcept;
		void MinVarToFilterCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingDepthWeightCutoffCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingLumSigmaCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingNormalSigmaCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingDepthSigmaCallback(const Support::ParamVariant& p) noexcept;
		void NumWaveletPassesCallback(const Support::ParamVariant& p) noexcept;
		void WaveletFilterCallback(const Support::ParamVariant& p) noexcept;

		inline static const char* COMPILED_CS[(int)SHADERS::COUNT] =
		{
			"SVGF_TemporalFilter_cs.cso",
			"SVGF_SpatialVariance_cs.cso",
			"SVGF_AtrousWaveletTransform_cs.cso",
			"SVGF_GaussianFilter_cs.cso"
		};
	};
}