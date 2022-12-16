#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "ReSTIR_GI_Common.h"

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
	struct ReSTIR_GI final
	{
		enum class SHADER_IN_RES
		{
			PREV_TEMPORAL_RESERVOIR_A,
			PREV_TEMPORAL_RESERVOIR_B,
			PREV_TEMPORAL_RESERVOIR_C,
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
			COUNT
		};

		ReSTIR_GI() noexcept;
		~ReSTIR_GI() noexcept;

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
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbTemporalPass) / sizeof(DWORD), sizeof(cbSpatialPass) / sizeof(DWORD));

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
		int m_currTemporalReservoirIdx = 0;
		bool m_isTemporalReservoirValid = false;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R16G16_FLOAT;
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
			COUNT
		};

		Core::DescriptorTable m_descTable;

		struct DefaultParamVals
		{
			static constexpr float MaxPlaneDist = 0.35f;
			static constexpr float NormalExp = 2.0f;
			static constexpr int ValidationPeriod = 0;
		};

		cbTemporalPass m_cbTemporal;
		cbSpatialPass m_cbSpatial;
		bool m_doSpatialResampling = true;
		int m_validationPeriod = 0;
		int m_validationFrame = 1;
		int m_sampleIdx = 0;
		uint32_t m_internalCounter = 0;

		void DoTemporalResamplingCallback(const Support::ParamVariant& p) noexcept;
		void DoSpatialResamplingCallback(const Support::ParamVariant& p) noexcept;
		void PdfCorrectionCallback(const Support::ParamVariant& p) noexcept;
		void MaxPlaneDistCallback(const Support::ParamVariant& p) noexcept;
		void ValidationPeriodCallback(const Support::ParamVariant& p) noexcept;
		void NormalExpCallback(const Support::ParamVariant& p) noexcept;
		void CheckerboardTracingCallback(const Support::ParamVariant& p) noexcept;

		enum class SHADERS
		{
			TEMPORAL_PASS,
			SPATIAL_PASS,
			VALIDATION,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"ReSTIR_GI_TemporalPass_cs.cso", 
			"ReSTIR_GI_SpatialPass_cs.cso", 
			"ReSTIR_GI_Validation_cs.cso" 
		};

		// shader reload
		void ReloadTemporalPass() noexcept;
		void ReloadSpatialPass() noexcept;
		void ReloadValidationPass() noexcept;
	};
}