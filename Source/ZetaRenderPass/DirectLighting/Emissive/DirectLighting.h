#pragma once

#include "../../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "DirectLighting_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
	struct RenderNodeHandle;
}

namespace ZetaRay::Support
{
	struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
	// a compute shader that estimates lumen for each emissive triangle
	struct EmissiveTriangleLumen final : public RenderPassBase
	{
		enum class SHADER_OUT_RES
		{
			TRI_LUMEN,
			READBACK_BUFF,
			COUNT
		};

		EmissiveTriangleLumen();
		~EmissiveTriangleLumen();

		Core::GpuMemory::DefaultHeapBuffer& GetLumenBuffer() { return m_lumen; }
		Core::GpuMemory::ReadbackHeapBuffer& GetReadbackBuffer() { return m_readback; }

		void Init();
		bool IsInitialized() const { return m_psos[0] != nullptr; };
		void Reset();
		void Update();
		void Render(Core::CommandList& cmdList);

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 2;
		static constexpr int NUM_UAV = 1;
		static constexpr int NUM_GLOBS = 2;
		static constexpr int NUM_CONSTS = sizeof(cb_ReSTIR_DI_EstimateTriLumen) / sizeof(DWORD);

		enum class SHADERS
		{
			ESTIMATE_TRIANGLE_LUMEN,
			COUNT
		};

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"EstimateTriLumen_cs.cso"
		};

		Core::GpuMemory::DefaultHeapBuffer m_halton;
		Core::GpuMemory::DefaultHeapBuffer m_lumen;
		Core::GpuMemory::ReadbackHeapBuffer m_readback;
		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
		uint32_t m_currNumTris;
	};

	struct EmissiveTriangleAliasTable
	{
		enum class SHADER_OUT_RES
		{
			ALIAS_TABLE,
			COUNT
		};

		EmissiveTriangleAliasTable() = default;
		~EmissiveTriangleAliasTable() = default;

		Core::GpuMemory::DefaultHeapBuffer& GetOutput(SHADER_OUT_RES i)
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_aliasTable;
		}

		void Update(Core::GpuMemory::ReadbackHeapBuffer* readback);
		void SetEmissiveTriPassHandle(Core::RenderNodeHandle& emissiveTriHandle);
		void Render(Core::CommandList& cmdList);

	private:
		Core::GpuMemory::DefaultHeapBuffer m_aliasTable;
		Core::GpuMemory::UploadHeapBuffer m_aliasTableUpload;
		Core::GpuMemory::ReadbackHeapBuffer* m_readback = nullptr;
		uint32_t m_currNumTris = 0;
		int m_emissiveTriHandle = -1;
	};

	struct DirectLighting final : public RenderPassBase
	{
		enum class SHADER_OUT_RES
		{
			DENOISED,
			COUNT
		};

		DirectLighting();
		~DirectLighting();

		void Init();
		bool IsInitialized() const { return m_psos[0] != nullptr; };
		void Reset();
		void OnWindowResized();

		const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
		{
			Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
			return m_denoised;
		}

		void Update();
		void Render(Core::CommandList& cmdList);

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 5;
		static constexpr int NUM_UAV = 1;
		static constexpr int NUM_GLOBS = 4;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_ReSTIR_DI_SpatioTemporal) / sizeof(DWORD),
			Math::Max(sizeof(cb_ReSTIR_DI_DNSR_Temporal) / sizeof(DWORD), sizeof(cb_ReSTIR_DI_DNSR_Spatial) / sizeof(DWORD)));

		void CreateOutputs();

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_UINT;
			static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R32G32_UINT;
			static constexpr DXGI_FORMAT COLOR_A = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT COLOR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
			static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
		};

		enum class DESC_TABLE
		{
			RESERVOIR_0_A_SRV,
			RESERVOIR_0_B_SRV,
			RESERVOIR_0_A_UAV,
			RESERVOIR_0_B_UAV,
			//
			RESERVOIR_1_A_SRV,
			RESERVOIR_1_B_SRV,
			RESERVOIR_1_A_UAV,
			RESERVOIR_1_B_UAV,
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
			static constexpr int NUM_SAMPLE_SETS = 128;
			static constexpr int SAMPLE_SET_SIZE = 512;
			static constexpr float EMISSIVE_SET_MEM_BUDGET_MB = 1.5;
			static constexpr int MIN_NUM_LIGHTS_PRESAMPLING = int((EMISSIVE_SET_MEM_BUDGET_MB * 1024 * 1024) / sizeof(LightSample));
			static constexpr int M_MAX = 25;
			static constexpr int DNSR_TSPP_DIFFUSE = 16;
			static constexpr int DNSR_TSPP_SPECULAR = 16;
		};

		enum class SHADERS
		{
			PRESAMPLING,
			SPATIO_TEMPORAL,
			SPATIO_TEMPORAL_LIGHT_PRESAMPLING,
			DNSR_TEMPORAL,
			DNSR_SPATIAL,
			COUNT
		};

		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"ReSTIR_DI_Presample_cs.cso",
			"ReSTIR_DI_SpatioTemporal_cs.cso",
			"ReSTIR_DI_SpatioTemporal_LP_cs.cso",
			"ReSTIR_DI_DNSR_Temporal_cs.cso",
			"ReSTIR_DI_DNSR_Spatial_cs.cso"
		};

		struct Reservoir
		{
			// Texture2D<uint4>: ((Li.g << 16 | Li.r), (M << 16 | Li.b), (bary.y << 16 | bary.x), W)
			Core::GpuMemory::Texture ReservoirA;
			// Texture2D<uint>: (lightIdx)
			Core::GpuMemory::Texture ReservoirB;
		};

		struct DenoiserCache
		{
			Core::GpuMemory::Texture Diffuse;
			Core::GpuMemory::Texture Specular;
		};

		Core::DescriptorTable m_descTable;
		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

		Reservoir m_temporalReservoir[2];
		Core::GpuMemory::Texture m_colorA;
		Core::GpuMemory::Texture m_colorB;
		DenoiserCache m_dnsrCache[2];
		Core::GpuMemory::Texture m_denoised;
		Core::GpuMemory::DefaultHeapBuffer m_sampleSets;

		uint32_t m_currNumTris = 0;
		int m_currTemporalIdx = 0;
		bool m_isTemporalReservoirValid = false;
		bool m_isDnsrTemporalCacheValid = false;
		bool m_doTemporalResampling = true;
		bool m_doSpatialResampling = true;
		
		cb_ReSTIR_DI_SpatioTemporal m_cbSpatioTemporal;
		cb_ReSTIR_DI_DNSR_Temporal m_cbDnsrTemporal;
		cb_ReSTIR_DI_DNSR_Spatial m_cbDnsrSpatial;

		// param callbacks
		void TemporalResamplingCallback(const Support::ParamVariant& p);
		void SpatialResamplingCallback(const Support::ParamVariant& p);
		void MaxTemporalMCallback(const Support::ParamVariant& p);
		void MaxRoughessExtraBrdfSamplingCallback(const Support::ParamVariant& p);
		void DenoiseCallback(const Support::ParamVariant& p);
		void TsppDiffuseCallback(const Support::ParamVariant& p);
		void TsppSpecularCallback(const Support::ParamVariant& p);
		void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p);
		void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p);
		//void FireflyFilterCallback(const Support::ParamVariant& p);

		// shader reload
		void ReloadSpatioTemporal();
		void ReloadDnsrTemporal();
		void ReloadDnsrSpatial();
	};
}