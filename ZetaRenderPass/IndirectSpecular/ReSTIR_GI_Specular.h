#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "ReSTIR_GI_Specular_Common.h"

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
	struct ReSTIR_GI_Specular final
	{
		enum class SHADER_IN_RES
		{
			PREV_TEMPORAL_RESERVOIR_A,
			PREV_TEMPORAL_RESERVOIR_B,
			PREV_TEMPORAL_RESERVOIR_C,
			PREV_TEMPORAL_RESERVOIR_D,
			PREV_DNSR_CACHE,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			TEMPORAL_RESERVOIR_A,
			TEMPORAL_RESERVOIR_B,
			TEMPORAL_RESERVOIR_C,
			TEMPORAL_RESERVOIR_D,
			SPATIAL_RESERVOIR_A,
			SPATIAL_RESERVOIR_B,
			SPATIAL_RESERVOIR_D,
			CURR_DNSR_CACHE,
			COUNT
		};

		ReSTIR_GI_Specular() noexcept;
		~ReSTIR_GI_Specular() noexcept;

		void Init() noexcept;
		bool IsInitialized() const noexcept { return m_psos[0] != nullptr; };
		void Reset() noexcept;
		float GetRoughnessCutoff() const noexcept { return m_cbTemporal.RoughnessCutoff; }
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
			case SHADER_IN_RES::PREV_TEMPORAL_RESERVOIR_D:
				return m_temporalReservoirs[1 - m_currTemporalReservoirIdx].ReservoirD;
			case SHADER_IN_RES::PREV_DNSR_CACHE:
				return m_dnsrTemporalCache[1 - m_currTemporalReservoirIdx];
			}

			Assert(false, "Unreachable case.");
			return m_temporalReservoirs[0].ReservoirC;
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
			case SHADER_OUT_RES::TEMPORAL_RESERVOIR_D:
				return m_temporalReservoirs[m_currTemporalReservoirIdx].ReservoirD;
			case SHADER_OUT_RES::SPATIAL_RESERVOIR_A:
				return m_spatialReservoir.ReservoirA;
			case SHADER_OUT_RES::SPATIAL_RESERVOIR_B:
				return m_spatialReservoir.ReservoirB;
			case SHADER_OUT_RES::SPATIAL_RESERVOIR_D:
				return m_spatialReservoir.ReservoirD;
			case SHADER_OUT_RES::CURR_DNSR_CACHE:
				return m_dnsrTemporalCache[m_currTemporalReservoirIdx];
			}

			Assert(false, "Unreachable case.");
			return m_temporalReservoirs[0].ReservoirC;
		}

		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 8;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 9;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_RGI_Spec_Temporal) / sizeof(DWORD), sizeof(cb_RGI_Spec_Spatial) / sizeof(DWORD));

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
			// Texture2D<half4>: (BrdfCosTheta, W)
			Core::Texture ReservoirD;
		};

		Reservoir m_temporalReservoirs[2];
		Reservoir m_spatialReservoir;
		Core::Texture m_dnsrTemporalCache[2];
		int m_currTemporalReservoirIdx = 0;
		bool m_isTemporalReservoirValid = false;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R16G16_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_D = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			TEMPORAL_RESERVOIR_0_A_SRV,
			TEMPORAL_RESERVOIR_0_B_SRV,
			TEMPORAL_RESERVOIR_0_C_SRV,
			TEMPORAL_RESERVOIR_0_D_SRV,
			TEMPORAL_RESERVOIR_0_A_UAV,
			TEMPORAL_RESERVOIR_0_B_UAV,
			TEMPORAL_RESERVOIR_0_C_UAV,
			TEMPORAL_RESERVOIR_0_D_UAV,
			//
			TEMPORAL_RESERVOIR_1_A_SRV,
			TEMPORAL_RESERVOIR_1_B_SRV,
			TEMPORAL_RESERVOIR_1_C_SRV,
			TEMPORAL_RESERVOIR_1_D_SRV,
			TEMPORAL_RESERVOIR_1_A_UAV,
			TEMPORAL_RESERVOIR_1_B_UAV,
			TEMPORAL_RESERVOIR_1_C_UAV,
			TEMPORAL_RESERVOIR_1_D_UAV,
			//
			SPATIAL_RESERVOIR_0_A_SRV,
			SPATIAL_RESERVOIR_0_B_SRV,
			//SPATIAL_RESERVOIR_0_C_SRV,
			SPATIAL_RESERVOIR_0_D_SRV,
			SPATIAL_RESERVOIR_0_A_UAV,
			SPATIAL_RESERVOIR_0_B_UAV,
			//SPATIAL_RESERVOIR_0_C_UAV,
			SPATIAL_RESERVOIR_0_D_UAV,
			//
			DNSR_TEMPORAL_CACHE_0_SRV,
			DNSR_TEMPORAL_CACHE_1_SRV,
			DNSR_TEMPORAL_CACHE_0_UAV,
			DNSR_TEMPORAL_CACHE_1_UAV,
			//
			COUNT
		};

		Core::DescriptorTable m_descTable;

		struct DefaultParamVals
		{
			static constexpr float RoughnessCutoff = 0.6f;
			static constexpr float TemporalHitDistSigmaScale = 0.94f;
			static constexpr float MinRoughnessResample = 0.1f;
			static constexpr int TemporalM_max = 20;
			static constexpr float SpatialHitDistSigmaScale = 3.0f;
			static constexpr int SpatialM_max = 15;
			static constexpr int SpatialResampleRadius = 16;
			static constexpr int SpatialResampleNumIter = 8;
			static constexpr int DNSRTspp = 32;
			static constexpr float DNSRHitDistSigmaScale = 0.8f;
			static constexpr float DNSRViewAngleExp = 0.5f;
			static constexpr float DNSRRoughnessExpScale = 0.85f;
		};

		cb_RGI_Spec_Temporal m_cbTemporal;
		cb_RGI_Spec_Spatial m_cbSpatial;
		cbDNSR m_cbDNSR;
		int m_sampleIdx = 0;
		uint32_t m_internalCounter = 0;

		void DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept;
		void DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept;
		void PdfCorrectionCallback(const Support::ParamVariant& p) noexcept;
		void RoughnessCutoffCallback(const Support::ParamVariant& p) noexcept;
		void MaxTemporalMCallback(const Support::ParamVariant& p) noexcept;
		void MaxSpatialMCallback(const Support::ParamVariant& p) noexcept;
		void MinRoughnessResample(const Support::ParamVariant& p) noexcept;
		void TemporalHistDistSigmaScaleCallback(const Support::ParamVariant& p) noexcept;
		void SpatialHistDistSigmaScaleCallback(const Support::ParamVariant& p) noexcept;
		void NumIterationsCallback(const Support::ParamVariant& p) noexcept;
		void SpatialRadiusCallback(const Support::ParamVariant& p) noexcept;
		void CheckerboardingCallback(const Support::ParamVariant& p) noexcept;
		void DoDenoisingCallback(const Support::ParamVariant& p) noexcept;
		void TsppCallback(const Support::ParamVariant& p) noexcept;
		void DNSRViewAngleExpCallback(const Support::ParamVariant& p) noexcept;
		void DNSRRoughnessExpScaleCallback(const Support::ParamVariant& p) noexcept;

		enum class SHADERS
		{
			TEMPORAL_RESAMPLE,
			SPATIAL_RESAMPLE,
			DNSR,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"ReSTIR_GI_Specular_Temporal_cs.cso", 
			"ReSTIR_GI_Specular_Spatial_cs.cso",
			"SpecularDNSR_Temporal_cs.cso"
		};

		// shader reload
		void ReloadTemporalPass() noexcept;
		void ReloadSpatialPass() noexcept;
		void ReloadDNSR() noexcept;
	};
}