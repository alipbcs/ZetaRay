#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "ReSTIR_DI_Common.h"

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
	struct ReSTIR_DI final
	{
		enum class SHADER_OUT_RES
		{
			DENOISED,
			COUNT
		};

		ReSTIR_DI() noexcept;
		~ReSTIR_DI() noexcept;

		void Init() noexcept;
		bool IsInitialized() const { return m_psos[0] != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;

		const Core::Texture& GetOutput(SHADER_OUT_RES i) const
		{
			Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
			return m_dnsrFinal;
		}

		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 4;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 5;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_RDI_Temporal) / sizeof(DWORD), 
			Math::Max(sizeof(cb_RDI_Spatial) / sizeof(DWORD), 
				Math::Max(sizeof(cb_RDI_DNSR_Temporal) / sizeof(DWORD), sizeof(cb_RDI_DNSR_Spatial) / sizeof(DWORD))));

		RpObjects s_rpObjs;

		void CreateOutputs() noexcept;

		Core::RootSignature m_rootSig;
		
		struct Reservoir
		{
			// Texture2D<uint4>: (W, (wi.y << 16 | wi.x), (Li.g << 16 | Li.r), (M << 16 | Li.b))
			Core::Texture ReservoirA;
			// Texture2D<float>: (w_sum)
			Core::Texture ReservoirB;
		};

		struct DenoiserCache
		{
			Core::Texture Diffuse;
			Core::Texture Specular;
		};

		Reservoir m_temporalReservoirs[2];
		Reservoir m_spatialReservoir;
		DenoiserCache m_dnsrCache[2];
		Core::Texture m_dnsrFinal;
		int m_currTemporalIdx = 0;
		bool m_doTemporalResampling = true;
		bool m_isTemporalReservoirValid = false;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_UINT;
			static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R32_FLOAT;
			static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			TEMPORAL_RESERVOIR_0_A_SRV,
			TEMPORAL_RESERVOIR_0_B_SRV,
			TEMPORAL_RESERVOIR_0_A_UAV,
			TEMPORAL_RESERVOIR_0_B_UAV,
			//
			TEMPORAL_RESERVOIR_1_A_SRV,
			TEMPORAL_RESERVOIR_1_B_SRV,
			TEMPORAL_RESERVOIR_1_A_UAV,
			TEMPORAL_RESERVOIR_1_B_UAV,
			//
			SPATIAL_RESERVOIR_A_SRV,
			SPATIAL_RESERVOIR_A_UAV,
			//
			DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV,
			DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV,
			DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV,
			DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV,
			DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV,
			DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV,
			DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV,
			DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV,
			DNSR_FINAL_UAV,
			//
			COUNT
		};

		Core::DescriptorTable m_descTable;

		struct DefaultParamVals
		{
			static constexpr float MinRoughnessToResample = 0.1f;
			static constexpr int TemporalM_max = 12;
			static constexpr int DNSRTspp_Diffuse = 8;
			static constexpr int DNSRTspp_Specular = 20;
		};

		cb_RDI_Temporal m_cbTemporalResample;
		cb_RDI_Spatial m_cbSpatialResample;
		cb_RDI_DNSR_Temporal m_cbDNSRTemporal;
		cb_RDI_DNSR_Spatial m_cbDNSRSpatial;
		int m_sampleIdx = 0;
		uint32_t m_internalCounter = 0;

		void DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept;
		void DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept;
		void MaxTemporalMCallback(const Support::ParamVariant& p) noexcept;
		void CheckerboardingCallback(const Support::ParamVariant& p) noexcept;
		void MinRoughnessResampleCallback(const Support::ParamVariant& p) noexcept;
		void SetReservoirPrefilteringEnablementCallback(const Support::ParamVariant& p) noexcept;
		void DoDenoisingCallback(const Support::ParamVariant& p) noexcept;
		void TsppDiffuseCallback(const Support::ParamVariant& p) noexcept;
		void TsppSpecularCallback(const Support::ParamVariant& p) noexcept;
		void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p) noexcept;
		void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p) noexcept;

		enum class SHADERS
		{
			TEMPORAL_RESAMPLE,
			SPATIAL_RESAMPLE,
			DNSR_TEMPORAL,
			DNSR_SPATIAL,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"ReSTIR_DI_Temporal_cs.cso", 
			"ReSTIR_DI_Spatial_cs.cso",
			"DirectDNSR_Temporal_cs.cso",
			"DirectDNSR_SpatialFilter_cs.cso"
		};

		// shader reload
		void ReloadTemporalPass() noexcept;
		void ReloadSpatialPass() noexcept;
		void ReloadDNSRTemporal() noexcept;
		void ReloadDNSRSpatial() noexcept;
	};
}