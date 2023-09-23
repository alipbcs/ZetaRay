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
			PREV_DNSR_CACHE,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			CURR_DNSR_CACHE,
			COUNT
		};

		ReSTIR_GI_Specular();
		~ReSTIR_GI_Specular();

		void Init();
		bool IsInitialized() const { return m_psos[0] != nullptr; };
		void Reset();
		float GetRoughnessCutoff() const { return m_cbTemporal.RoughnessCutoff; }
		void OnWindowResized();

		const Core::GpuMemory::Texture& GetInput(SHADER_IN_RES i) const
		{
			Assert(i == SHADER_IN_RES::PREV_DNSR_CACHE, "Invalid shader input.");
			return m_dnsrTemporalCache[1 - m_currTemporalReservoirIdx];
		}

		const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
		{
			Assert(i == SHADER_OUT_RES::CURR_DNSR_CACHE, "Invalid shader output.");
			return m_dnsrTemporalCache[m_currTemporalReservoirIdx];
		}

		void Render(Core::CommandList& cmdList);

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 8;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 9;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_RGI_Spec_Temporal) / sizeof(DWORD), sizeof(cb_RGI_Spec_Spatial) / sizeof(DWORD));

		RpObjects s_rpObjs;

		void CreateOutputs();

		Core::RootSignature m_rootSig;
		
		struct Reservoir
		{
			// Texture2D<float4>: (Pos, w_sum)
			Core::GpuMemory::Texture ReservoirA;
			// Texture2D<half4>: (Li, M)
			Core::GpuMemory::Texture ReservoirB;
			// Texture2D<half2>: (Normal)
			Core::GpuMemory::Texture ReservoirC;
			// Texture2D<half4>: (BrdfCosTheta, W)
			Core::GpuMemory::Texture ReservoirD;
		};

		Reservoir m_temporalReservoirs[2];
		Reservoir m_spatialReservoir;
		Core::GpuMemory::Texture m_dnsrTemporalCache[2];
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
			SPATIAL_RESERVOIR_0_D_SRV,
			SPATIAL_RESERVOIR_0_A_UAV,
			SPATIAL_RESERVOIR_0_B_UAV,
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
			static constexpr float DNSRViewAngleExp = 0.35f;
			static constexpr float DNSRRoughnessExpScale = 0.9f;
		};

		cb_RGI_Spec_Temporal m_cbTemporal;
		cb_RGI_Spec_Spatial m_cbSpatial;
		cb_RGI_Spec_DNSR m_cbDNSR;
		int m_sampleIdx = 0;
		uint32_t m_internalCounter = 0;

		void DoTemporalResamplingCallback(const Support::ParamVariant& p);
		void DoSpatialResamplingCallback(const Support::ParamVariant& p);
		void RoughnessCutoffCallback(const Support::ParamVariant& p);
		void MaxTemporalMCallback(const Support::ParamVariant& p);
		void MinRoughnessResample(const Support::ParamVariant& p);
		void TemporalHistDistSigmaScaleCallback(const Support::ParamVariant& p);
		void SpatialHistDistSigmaScaleCallback(const Support::ParamVariant& p);
		void NumIterationsCallback(const Support::ParamVariant& p);
		void SpatialRadiusCallback(const Support::ParamVariant& p);
		void CheckerboardingCallback(const Support::ParamVariant& p);
		void DoDenoisingCallback(const Support::ParamVariant& p);
		void TsppCallback(const Support::ParamVariant& p);
		void DNSRViewAngleExpCallback(const Support::ParamVariant& p);
		void DNSRRoughnessExpScaleCallback(const Support::ParamVariant& p);

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
		void ReloadTemporalPass();
		void ReloadSpatialPass();
		void ReloadDNSR();
	};
}