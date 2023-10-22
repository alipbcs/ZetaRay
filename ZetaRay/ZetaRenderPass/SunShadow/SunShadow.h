#pragma once

#include "../RenderPass.h"
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
	struct SunShadow final : public RenderPassBase
	{
		enum class SHADER_OUT_RES
		{
			DENOISED,
			COUNT
		};

		SunShadow();
		~SunShadow();

		void Init();
		bool IsInitialized() { return m_psos[(int)SHADERS::SHADOW_MASK] != nullptr; };
		void Reset();
		void OnWindowResized();
		const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_denoised;
		}
		void Render(Core::CommandList& cmdList);

	private:
		void CreateResources();

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 1;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 2;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbFFX_DNSR_Temporal) / sizeof(DWORD),
			sizeof(cbFFX_DNSR_Spatial) / sizeof(DWORD));

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT SHADOW_MASK = DXGI_FORMAT_R32_UINT;
			static constexpr DXGI_FORMAT THREAD_GROUP_METADATA = DXGI_FORMAT_R8_UINT;
			static constexpr DXGI_FORMAT MOMENTS = DXGI_FORMAT_R11G11B10_FLOAT;
			static constexpr DXGI_FORMAT TEMPORAL_CACHE = DXGI_FORMAT_R16G16_FLOAT;
			static constexpr DXGI_FORMAT DENOISED = DXGI_FORMAT_R16_FLOAT;
		};

		enum class DESC_TABLE
		{
			SHADOW_MASK_SRV,
			SHADOW_MASK_UAV,
			METADATA_SRV,
			METADATA_UAV,
			TEMPORAL_CACHE_0_SRV,
			TEMPORAL_CACHE_0_UAV,
			TEMPORAL_CACHE_1_SRV,
			TEMPORAL_CACHE_1_UAV,
			MOMENTS_UAV,
			DENOISED_UAV,
			COUNT
		};

		enum class SHADERS
		{
			SHADOW_MASK,
			DNSR_TEMPORAL_PASS,
			DNSR_SPATIAL_FILTER,
			COUNT
		};

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] =
		{
			"SunShadow_cs.cso",
			"ffx_denoiser_temporal_cs.cso",
			"ffx_denoiser_spatial_filter_cs.cso"
		};

		struct DefaultParamVals
		{
			static constexpr float EdgeStoppingNormalExp = 32.0f;
			static constexpr float EdgeStoppingShadowStdScale = 0.5f;
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
		Core::GpuMemory::Texture m_shadowMask;
		Core::GpuMemory::Texture m_metadata;
		Core::GpuMemory::Texture m_moments;
		Core::GpuMemory::Texture m_temporalCache[2];
		Core::GpuMemory::Texture m_denoised;
		Core::DescriptorTable m_descTable;
		int m_currTemporalIdx = 0;
		bool m_doSoftShadows = true;
		bool m_denoise = true;

		cbFFX_DNSR_Temporal m_temporalCB;
		cbFFX_DNSR_Spatial m_spatialCB;

		void DoSoftShadowsCallback(const Support::ParamVariant& p);
		void DenoiseCallback(const Support::ParamVariant& p);
		//void NumSpatialFilterPassesCallback(const Support::ParamVariant& p);
		void EdgeStoppingShadowStdScaleCallback(const Support::ParamVariant& p);
		void MinFilterVarianceCallback(const Support::ParamVariant& p);

		void ReloadDNSRTemporal();
		void ReloadDNSRSpatial();
		void ReloadSunShadowTrace();
	};
}