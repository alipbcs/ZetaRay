#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "IndirectLighting_Common.h"

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
	struct IndirectLighting final : public RenderPassBase
	{
		enum class SHADER_OUT_RES
		{
			DENOISED,
			COUNT
		};

		IndirectLighting();
		~IndirectLighting();

		void Init();
		bool IsInitialized() const { return m_psos[0] != nullptr; };
		void Reset();
		void OnWindowResized();
		void SetCurvatureDescHeapOffset(uint32_t descHeapOffset) { m_cbSpatioTemporal.CurvatureDescHeapIdx = descHeapOffset; }
		void SetLightPresamplingEnabled(bool b, int numSampleSets, int sampleSetSize)
		{
			Assert(!b || (b && numSampleSets && sampleSetSize), "presampling is enabled, but number of sample sets is zero.");

			m_preSampling = b;
			m_cbSpatioTemporal.NumSampleSets = b ? (uint16_t)numSampleSets : 0;
			m_cbSpatioTemporal.SampleSetSize = b ? (uint16_t)sampleSetSize : 0;
		}
		void SetLightVoxelGridParams(const Math::uint3& dim, const Math::float3& extents, float offset_y)
		{
			m_cbSpatioTemporal.GridDim_x = (uint16_t)dim.x;
			m_cbSpatioTemporal.GridDim_y = (uint16_t)dim.y;
			m_cbSpatioTemporal.GridDim_z = (uint16_t)dim.z;

			m_cbSpatioTemporal.Extents_x = extents.x;
			m_cbSpatioTemporal.Extents_y = extents.y;
			m_cbSpatioTemporal.Extents_z = extents.z;

			m_cbSpatioTemporal.Offset_y = offset_y;
		}
		const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
		{
			Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
			return m_denoised;
		}
		void Render(Core::CommandList& cmdList);

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 9;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 10;
		static constexpr int NUM_CONSTS = (int)sizeof(cb_ReSTIR_GI_SpatioTemporal) / sizeof(DWORD);

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_UINT;
			static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R32G32B32A32_FLOAT;
			static constexpr DXGI_FORMAT COLOR_A = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT COLOR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			RESERVOIR_0_A_SRV,
			RESERVOIR_0_B_SRV,
			RESERVOIR_0_C_SRV,
			RESERVOIR_0_A_UAV,
			RESERVOIR_0_B_UAV,
			RESERVOIR_0_C_UAV,
			//
			RESERVOIR_1_A_SRV,
			RESERVOIR_1_B_SRV,
			RESERVOIR_1_C_SRV,
			RESERVOIR_1_A_UAV,
			RESERVOIR_1_B_UAV,
			RESERVOIR_1_C_UAV,
			//
			COLOR_A_SRV,
			COLOR_A_UAV,
			COLOR_B_SRV,
			COLOR_B_UAV,
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

		struct DefaultParamVals
		{
			static constexpr uint16_t M_MAX = 2;
			static constexpr int DNSR_TSPP_DIFFUSE = 32;
			static constexpr int DNSR_TSPP_SPECULAR = 20;
		};

		enum class SHADERS
		{
			SPATIO_TEMPORAL,
			SPATIO_TEMPORAL_LVG,
			SPATIO_TEMPORAL_NPS,
			DNSR_TEMPORAL,
			DNSR_SPATIAL,
			COUNT
		};

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"ReSTIR_GI_cs.cso",
			"ReSTIR_GI_LVG_cs.cso",
			"ReSTIR_GI_NPS_cs.cso",
			"IndirectDnsr_Temporal_cs.cso",
			"IndirectDnsr_Spatial_cs.cso"
		};

		struct Reservoir
		{
			// Texture2D<uint4>: (pos, ID)
			Core::GpuMemory::Texture ReservoirA;
			// Texture2D<half4>: (Lo, M)
			Core::GpuMemory::Texture ReservoirB;
			// Texture2D<uint4>: (w_sum, W, normal)
			Core::GpuMemory::Texture ReservoirC;
		};

		struct DenoiserCache
		{
			Core::GpuMemory::Texture Diffuse;
			Core::GpuMemory::Texture Specular;
		};

		void CreateOutputs();

		// param callbacks
		void NumBouncesCallback(const Support::ParamVariant& p);
		void StochasticMultibounceCallback(const Support::ParamVariant& p);
		void RussianRouletteCallback(const Support::ParamVariant& p);
		void TemporalResamplingCallback(const Support::ParamVariant& p);
		void SpatialResamplingCallback(const Support::ParamVariant& p);
		void MaxTemporalMCallback(const Support::ParamVariant& p);
		void DenoiseCallback(const Support::ParamVariant& p);
		void TsppDiffuseCallback(const Support::ParamVariant& p);
		void TsppSpecularCallback(const Support::ParamVariant& p);
		void FireflyFilterCallback(const Support::ParamVariant& p);
		void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p);
		void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p);

		// shader reload
		void ReloadSpatioTemporal();
		void ReloadDnsrTemporal();
		void ReloadDnsrSpatial();

		Core::DescriptorTable m_descTable;
		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		Reservoir m_temporalReservoir[2];
		Core::GpuMemory::Texture m_colorA;
		Core::GpuMemory::Texture m_colorB;
		DenoiserCache m_dnsrCache[2];
		Core::GpuMemory::Texture m_denoised;

		int m_currTemporalIdx = 0;
		bool m_isTemporalReservoirValid = false;
		bool m_isDnsrTemporalCacheValid = false;
		bool m_doTemporalResampling = true;
		bool m_doSpatialResampling = false;
		bool m_preSampling = false;

		cb_ReSTIR_GI_SpatioTemporal m_cbSpatioTemporal;
		cbIndirectDnsrTemporal m_cbDnsrTemporal;
		cbIndirectDnsrSpatial m_cbDnsrSpatial;
	};
}