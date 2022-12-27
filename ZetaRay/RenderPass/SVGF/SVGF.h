#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "SVGF_Common.h"

namespace ZetaRay
{
	class CommandList;
	struct ParamVariant;

	namespace RenderPass
	{
		struct SVGF final
		{
			enum class SHADER_IN_RES
			{
				INDIRECT_LO,
				LINEAR_DEPTH_GRAD,
				COUNT
			};

			enum class SHADER_OUT_RES
			{
				TEMPORAL_CACHE_COL_LUM_A,
				TEMPORAL_CACHE_COL_LUM_B,
				TEMPORAL_CACHE_TSPP_A,
				TEMPORAL_CACHE_TSPP_B,
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
			const Texture& GetOutput(SHADER_OUT_RES i) const noexcept
			{
				Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
				
				switch (i)
				{
				case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A:
					return m_temporalCacheColLum[0];
				case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B:
					return m_temporalCacheColLum[1];
				case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_TSPP_A:
					return m_temporalCacheTSPP[0];
				case SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_TSPP_B:
					return m_temporalCacheTSPP[1];
				default:
					if (!m_filterSpatialVariance)
						return m_spatialLumVar;
					else
						return m_spatialLumVarFiltered;
				}
			}
			void Render(CommandList& cmdList) noexcept;

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
				TEMPORAL_CACHE_TSPP_A_SRV,
				TEMPORAL_CACHE_TSPP_A_UAV,
				TEMPORAL_CACHE_TSPP_B_SRV,
				TEMPORAL_CACHE_TSPP_B_UAV,
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
				static constexpr float BilinearNormalScale = 1.1f;
				static constexpr float BilinearNormalExp = 32.0f;
				static constexpr float BilinearDepthScale = 3.0f;
				static constexpr float BilinearDepthCutoff = 0.5f;
				//static constexpr bool NeighborhoodClamping = true;
				//static constexpr float NeighborhoodClampingStdScale = 0.6f;
				//static constexpr float NeighborhoodClampingMinStd = 0.05f;
				//static constexpr float NeighborhoodClampingTSPPAdj = 4.0f;
				static constexpr float MinLumVariance = 0.1f;
				static constexpr float MinConsistentWeight = 1e-3f;
				static constexpr int SpatialVarianceRadius = 4;
				static constexpr float MinVarianceToFilter = 0.0f;
				static constexpr float EdgeStoppingDepthWeightCutoff = 0.2f;
				static constexpr float EdgeStoppingLumSigma = 4.0f;
				static constexpr float EdgeStoppingNormalSigma = 128.0f;
				static constexpr float EdgeStoppingDepthSigma = 1.0f;
			};

			// all the shaders use the same root signature
			RootSignature m_rootSig;
			ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };

			uint32_t m_inputGpuHeapIndices[(int)SHADER_IN_RES::COUNT];

			Texture m_temporalCacheColLum[2];
			Texture m_temporalCacheTSPP[2];
			Texture m_spatialLumVar;
			Texture m_spatialLumVarFiltered;

			// descriptor-table containing all the needed descriptors
			DescriptorTable m_descTable;
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
			void MaxTSPPCallback(const ParamVariant& p) noexcept;
			void MinTSPPForTemporalVarCallback(const ParamVariant& p) noexcept;
			void BilateralNormalScaleCallback(const ParamVariant& p) noexcept;
			void BilateralNormalExpCallback(const ParamVariant& p) noexcept;
			void BilateralDepthScaleCallback(const ParamVariant& p) noexcept;
			void BilateralDepthCutoffCallback(const ParamVariant& p) noexcept;
			//void NeighborhoodClampingCallback(const ParamVariant& p) noexcept;
			//void NeighborhoodClampingStdScaleCallback(const ParamVariant& p) noexcept;
			//void NeighborhoodClampingMinStdCallback(const ParamVariant& p) noexcept;
			//void NeighborhoodClampingTSPPAdjCallback(const ParamVariant& p) noexcept;
			void MinLumVarCallback(const ParamVariant& p) noexcept;
			void MinConsistentWeightCallback(const ParamVariant& p) noexcept;
			void SpatialVarRadiusCallback(const ParamVariant& p) noexcept;
			void FilterSpatialVarCallback(const ParamVariant& p) noexcept;
			void MinVarToFilterCallback(const ParamVariant& p) noexcept;
			void EdgeStoppingDepthWeightCutoffCallback(const ParamVariant& p) noexcept;
			void EdgeStoppingLumSigmaCallback(const ParamVariant& p) noexcept;
			void EdgeStoppingNormalSigmaCallback(const ParamVariant& p) noexcept;
			void EdgeStoppingDepthSigmaCallback(const ParamVariant& p) noexcept;
			void NumWaveletPassesCallback(const ParamVariant& p) noexcept;
			void WaveletFilterCallback(const ParamVariant& p) noexcept;

			inline static const char* COMPILED_CS[(int)SHADERS::COUNT] =
			{
				"SVGF_TemporalFilter_cs.cso",
				"SVGF_SpatialVariance_cs.cso",
				"SVGF_AtrousWaveletTransform_cs.cso",
				"SVGF_GaussianFilter_cs.cso"
			};
		};
	}
}