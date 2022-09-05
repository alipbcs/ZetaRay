#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "STAD_Common.h"

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
	struct STAD final
	{
		enum class SHADER_IN_RES
		{
			INDIRECT_LI,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			TEMPORAL_CACHE_PRE_IN,
			TEMPORAL_CACHE_PRE_OUT,
			TEMPORAL_CACHE_POST_OUT,
			COUNT
		};

		STAD() noexcept;
		~STAD() noexcept;

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

			if (i == SHADER_OUT_RES::TEMPORAL_CACHE_PRE_IN)
				return m_temporalCache[(m_currTemporalCacheOutIdx + 1) & 0x1];
			else if(i == SHADER_OUT_RES::TEMPORAL_CACHE_PRE_OUT)
				return m_temporalCache[m_currTemporalCacheOutIdx];

			int outIdx = m_currTemporalCacheOutIdx;

			// each round of sptial filtering swaps input & output
			if (m_doSpatialFilter)
			{
				for (int i = 0; i < m_numSpatialFilterPasses; i++)
					outIdx = (outIdx + 1) & 0x1;
			}

			return m_temporalCache[outIdx];
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void CreateResources() noexcept;
		void InitParams() noexcept;
		void ReloadTemporalPass() noexcept;
		void ReloadSpatialFilter() noexcept;

		enum class SHADERS
		{
			TEMPORAL_PASS,
			SPATIAL_FILTER,
			COUNT
		};

		enum class DESC_TABLE
		{
			INDIRECT_LI_SRV,
			TEMPORAL_CACHE_A_SRV,
			TEMPORAL_CACHE_A_UAV,
			TEMPORAL_CACHE_B_SRV,
			TEMPORAL_CACHE_B_UAV,
			COUNT
		};

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 3;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr size_t NUM_CONSTS = std::max(sizeof(cbSTADSpatialFilter) / sizeof(DWORD), 
			sizeof(cbSTADTemporalFilter) / sizeof(DWORD));

		inline static RpObjects s_rpObjs;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		struct DefaultParamVals
		{
			static constexpr int NumSpatialPasses = 1;
			static constexpr int MaxTSPP = 32;
			static constexpr float BilinearMaxPlaneDist = 0.1f;
			static constexpr float BilinearNormalScale = 1.4f;
			static constexpr float BilinearNormalExp = 16.0f;
			static constexpr float EdgeStoppingMaxPlaneDist = 3.0f;
			static constexpr float EdgeStoppingNormalExp = 8.0f;
			static constexpr float FilterRadiusBase = 5e-2f;
			static constexpr float FilterRadiusScale = 1.0f;
		};

		// all the shaders use the same root signature
		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };

		uint32_t m_inputGpuHeapIndices[(int)SHADER_IN_RES::COUNT];

		Core::Texture m_temporalCache[2];
		int m_currTemporalCacheOutIdx = 1;

		// descriptor-table containing all the needed descriptors
		Core::DescriptorTable m_descTable;
		bool m_isTemporalCacheValid = false;
		int m_numSpatialFilterPasses = DefaultParamVals::NumSpatialPasses;
		bool m_doSpatialFilter = false;

		// constant buffers
		cbSTADTemporalFilter m_cbTemporalFilter;
		cbSTADSpatialFilter m_cbSpatialFilter;

		// parameter callbacks
		void MaxTSPPCallback(const Support::ParamVariant& p) noexcept;
		void BilinearMaxPlaneDistCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingMaxPlaneDistCallback(const Support::ParamVariant& p) noexcept;
		void BilinearNormalScaleCallback(const Support::ParamVariant& p) noexcept;
		void BilinearNormalExpCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingNormalExpCallback(const Support::ParamVariant& p) noexcept;
		void NumSpatialFilterPassesCallback(const Support::ParamVariant& p) noexcept;
		void SpatialFilterCallback(const Support::ParamVariant& p) noexcept;
		void FilterRadiusBaseCallback(const Support::ParamVariant& p) noexcept;
		void FilterRadiusScaleCallback(const Support::ParamVariant& p) noexcept;

		inline static const char* COMPILED_CS[(int)SHADERS::COUNT] =
		{
			"STAD_TemporalFilter_cs.cso",
			"STAD_AdaptiveSpatialFilter_cs.cso"
		};
	};
}