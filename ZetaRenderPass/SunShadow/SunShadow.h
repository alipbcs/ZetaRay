#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "SunShadow_Common.h"

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
	struct SunShadow final
	{
		enum class SHADER_IN_RES
		{
			TEMPORAL_CACHE_IN,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			TEMPORAL_CACHE_OUT_PRE,
			TEMPORAL_CACHE_OUT_POST,
			RAW_SHADOW_MASK,
			COUNT
		};

		SunShadow() noexcept;
		~SunShadow() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_psos[(int)SHADERS::SHADOW_MASK] != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		const Core::Texture& GetInput(SHADER_IN_RES i) const noexcept
		{
			Assert((int)i < (int)SHADER_IN_RES::COUNT, "out-of-bound access.");
			return m_temporalCache[1 - m_currTemporalCacheOutIdx];
		}
		const Core::Texture& GetOutput(SHADER_OUT_RES i) const noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");

			if (i == SHADER_OUT_RES::TEMPORAL_CACHE_OUT_PRE)
				return m_temporalCache[m_currTemporalCacheOutIdx];
			else if (i == SHADER_OUT_RES::RAW_SHADOW_MASK)
				return m_shadowMask;

			int idx = m_currTemporalCacheOutIdx;

			// each round of spatial filtering swaps input & output
			for (int j = 0; j < m_numSpatialPasses; j++)
				idx = 1 - idx;

			return m_temporalCache[idx];
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void ReloadDNSRTemporal() noexcept;
		void ReloadDNSRSpatial() noexcept;
		void CreateResources() noexcept;

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 4;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 5;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbFFX_DNSR_Temporal) / sizeof(DWORD),
			sizeof(cbFFX_DNSR_Spatial) / sizeof(DWORD));

		RpObjects s_rpObjs;
		Core::RootSignature m_rootSig;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT SHADOW_MASK = DXGI_FORMAT_R32_UINT;
			static constexpr DXGI_FORMAT THREAD_GROUP_METADATA = DXGI_FORMAT_R8_UINT;
			static constexpr DXGI_FORMAT MOMENTS = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT TEMPORAL_CACHE = DXGI_FORMAT_R16G16_FLOAT;
		};

		Core::Texture m_shadowMask;
		Core::Texture m_metadata;
		Core::Texture m_moments;
		Core::Texture m_temporalCache[2];
		int m_currTemporalCacheOutIdx = 0;
		int m_numSpatialPasses = 2;
		int m_oldNumSpatialPasses;
		bool m_doSoftShadows = true;

		enum class DESC_TABLE
		{
			SHADOW_MASK_SRV,
			SHADOW_MASK_UAV,
			METADATA_SRV,
			METADATA_UAV,
			TEMPORAL_CACHE_A_SRV,
			TEMPORAL_CACHE_A_UAV,
			TEMPORAL_CACHE_B_SRV,
			TEMPORAL_CACHE_B_UAV,
			MOMENTS_UAV,
			COUNT
		};

		Core::DescriptorTable m_descTable;

		enum class SHADERS
		{
			SHADOW_MASK,
			DNSR_TEMPORAL_PASS,
			DNSR_SPATIAL_FILTER,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"SunShadow_cs.cso",
			"ffx_denoiser_temporal_cs.cso",
			"ffx_denoiser_spatial_filter_cs.cso"
		};

		struct DefaultParamVals
		{
			static constexpr float EdgeStoppingNormalExp = 32.0f;
			static constexpr float MaxPlaneDist = 0.1f;
			static constexpr float EdgeStoppingShadowStdScale = 0.5f;
		};

		cbFFX_DNSR_Temporal m_temporalCB;
		cbFFX_DNSR_Spatial m_spatialCB;

		void DoSoftShadowsCallback(const Support::ParamVariant& p) noexcept;
		void NumSpatialFilterPassesCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingDepthSigmaCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingNormalExpCallback(const Support::ParamVariant& p) noexcept;
		void MaxPlaneDistCallback(const Support::ParamVariant& p) noexcept;
		void EdgeStoppingShadowStdScaleCallback(const Support::ParamVariant& p) noexcept;
	};
}