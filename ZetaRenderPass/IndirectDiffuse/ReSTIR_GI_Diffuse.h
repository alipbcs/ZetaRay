#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "ReSTIR_GI_Diffuse_Common.h"

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
	struct ReSTIR_GI_Diffuse final
	{
		enum class SHADER_IN_RES
		{
			PREV_TEMPORAL_RESERVOIR_A,
			PREV_TEMPORAL_RESERVOIR_B,
			PREV_TEMPORAL_RESERVOIR_C,
			PREV_DNSR_TEMPORAL_CACHE,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			TEMPORAL_RESERVOIR_A,
			TEMPORAL_RESERVOIR_B,
			TEMPORAL_RESERVOIR_C,
			SPATIAL_RESERVOIR_A,
			SPATIAL_RESERVOIR_B,
			SPATIAL_RESERVOIR_C,
			DNSR_TEMPORAL_CACHE_PRE_SPATIAL,
			DNSR_TEMPORAL_CACHE_POST_SPATIAL,
			COUNT
		};

		ReSTIR_GI_Diffuse() noexcept;
		~ReSTIR_GI_Diffuse() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_psos[0] != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;

		const Core::Texture& GetInput(SHADER_IN_RES i) const noexcept
		{
			switch (i)
			{
			case SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_A:
				return m_temporalReservoirs[1 - m_currTemporalReservoirIdx].ReservoirA;
			case SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_B:
				return m_temporalReservoirs[1 - m_currTemporalReservoirIdx].ReservoirB;
			case SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_C:
				return m_temporalReservoirs[1 - m_currTemporalReservoirIdx].ReservoirC;
			case SHADER_IN_RES::PREV_DNSR_TEMPORAL_CACHE:
				return m_temporalCache[1 - m_currDNSRTemporalIdx];
			}

			Assert(false, "Unreachable case.");
			return m_spatialReservoirs[1].ReservoirC;
		}

		const Core::Texture& GetOutput(SHADER_OUT_RES i) const noexcept
		{
			switch (i)
			{
			case SHADER_OUT_RES::TEMPORAL_RESERVOIR_A:
				return m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirA;
			case SHADER_OUT_RES::TEMPORAL_RESERVOIR_B:
				return m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirB;
			case SHADER_OUT_RES::TEMPORAL_RESERVOIR_C:
				return m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirC;
			case SHADER_OUT_RES::SPATIAL_RESERVOIR_A:
				return m_spatialReservoirs[1].ReservoirA;
			case SHADER_OUT_RES::SPATIAL_RESERVOIR_B:
				return m_spatialReservoirs[1].ReservoirB;
			case SHADER_OUT_RES::SPATIAL_RESERVOIR_C:
				return m_spatialReservoirs[1].ReservoirC;
			case SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_PRE_SPATIAL:
				return m_temporalCache[m_currDNSRTemporalIdx];
			case SHADER_OUT_RES::DNSR_TEMPORAL_CACHE_POST_SPATIAL:
				// ping ponging even number of times
				if((m_numDNSRSpatialFilterPasses & 0x1) == 0)
					return m_temporalCache[m_currDNSRTemporalIdx];
				
				return m_temporalCache[1 - m_currDNSRTemporalIdx];
			}

			Assert(false, "Unreachable case.");
			return m_spatialReservoirs[0].ReservoirC;
		}

		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 8;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 9;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_RGI_Diff_Temporal) / sizeof(DWORD), sizeof(cb_RGI_Diff_Spatial) / sizeof(DWORD));

		RpObjects s_rpObjs;

		void CreateOutputs() noexcept;

		Core::RootSignature m_rootSig;
		
		struct Reservoir
		{
			// Texture2D<float4>: (Pos, w_sum)
			Core::Texture ReservoirA;
			// Texture2D<half4>: (Li, M)
			Core::Texture ReservoirB;
			// Texture2D<half2>: (Normal)
			Core::Texture ReservoirC;
		};

		Reservoir m_temporalReservoirs[2];
		Reservoir m_spatialReservoirs[2];
		Core::Texture m_temporalCache[2];
		int m_currTemporalReservoirIdx = 0;
		bool m_isTemporalReservoirValid = false;
		int m_currDNSRTemporalIdx = 0;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R16G16_FLOAT;
			static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			TEMPORAL_RESERVOIR_0_A_SRV,
			TEMPORAL_RESERVOIR_0_B_SRV,
			TEMPORAL_RESERVOIR_0_C_SRV,
			TEMPORAL_RESERVOIR_0_A_UAV,
			TEMPORAL_RESERVOIR_0_B_UAV,
			TEMPORAL_RESERVOIR_0_C_UAV,
			//
			TEMPORAL_RESERVOIR_1_A_SRV,
			TEMPORAL_RESERVOIR_1_B_SRV,
			TEMPORAL_RESERVOIR_1_C_SRV,
			TEMPORAL_RESERVOIR_1_A_UAV,
			TEMPORAL_RESERVOIR_1_B_UAV,
			TEMPORAL_RESERVOIR_1_C_UAV,
			//
			SPATIAL_RESERVOIR_0_A_SRV,
			SPATIAL_RESERVOIR_0_B_SRV,
			SPATIAL_RESERVOIR_0_C_SRV,
			SPATIAL_RESERVOIR_0_A_UAV,
			SPATIAL_RESERVOIR_0_B_UAV,
			SPATIAL_RESERVOIR_0_C_UAV,
			//
			SPATIAL_RESERVOIR_1_A_SRV,
			SPATIAL_RESERVOIR_1_B_SRV,
			SPATIAL_RESERVOIR_1_C_SRV,
			SPATIAL_RESERVOIR_1_A_UAV,
			SPATIAL_RESERVOIR_1_B_UAV,
			SPATIAL_RESERVOIR_1_C_UAV,
			//
			TEMPORAL_CACHE_A_SRV,
			TEMPORAL_CACHE_A_UAV,
			TEMPORAL_CACHE_B_SRV,
			TEMPORAL_CACHE_B_UAV,
			//
			COUNT
		};

		Core::DescriptorTable m_descTable;

		struct DefaultParamVals
		{
			static constexpr float RGINormalExp = 1.5f;
			static constexpr float EdgeStoppingNormalExp = 2.0f;
			static constexpr int ValidationPeriod = 5;
			static constexpr int DNSRNumSpatialPasses = 1;
			static constexpr int DNSRMaxTSPP = 32;
		};

		cb_RGI_Diff_Temporal m_cbRGITemporal;
		cb_RGI_Diff_Spatial m_cbRGISpatial;
		cbDiffuseDNSRTemporal m_cbDNSRTemporal;
		cbDiffuseDNSRSpatial m_cbDNSRSpatial;
		int m_validationPeriod = 0;
		int m_validationFrame = 1;
		int m_sampleIdx = 0;
		uint32_t m_internalCounter = 0;
		int m_numDNSRSpatialFilterPasses = DefaultParamVals::DNSRNumSpatialPasses;

		void DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept;
		void DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept;
		void PdfCorrectionCallback(const Support::ParamVariant& p) noexcept;
		void ValidationPeriodCallback(const Support::ParamVariant& p) noexcept;
		void RGINormalExpCallback(const Support::ParamVariant& p) noexcept;
		void CheckerboardTracingCallback(const Support::ParamVariant& p) noexcept;
		void DNSRNumSpatialPassesCallback(const Support::ParamVariant& p) noexcept;
		void DNSRMaxTSPPCallback(const Support::ParamVariant& p) noexcept;
		void DNSRNormalExpCallback(const Support::ParamVariant& p) noexcept;

		enum class SHADERS
		{
			TEMPORAL_PASS,
			SPATIAL_PASS,
			VALIDATION,
			DIFFUSE_DNSR_TEMPORAL,
			DIFFUSE_DNSR_SPATIAL,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"ReSTIR_GI_Diffuse_Temporal_cs.cso", 
			"ReSTIR_GI_Diffuse_Spatial_cs.cso", 
			"ReSTIR_GI_Diffuse_Validation_cs.cso",
			"DiffuseDNSR_Temporal_cs.cso",
			"DiffuseDNSR_SpatialFilter_cs.cso" };

		// shader reload
		void ReloadRGITemporalPass() noexcept;
		void ReloadRGISpatialPass() noexcept;
		void ReloadValidationPass() noexcept;
		void ReloadDNSRTemporalPass() noexcept;
		void ReloadDNSRSpatialPass() noexcept;
	};
}