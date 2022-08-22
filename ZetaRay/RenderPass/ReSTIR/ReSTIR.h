#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
//#include "../GaussianFilter/GaussianFilter.h"
#include "../../SupportSystem/Param.h"
#include "ReSTIR_Common.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct ReSTIR final
		{
			enum class SHADER_IN_DESC
			{
				LINEAR_DEPTH_GRADIENT,
				COUNT
			};

			enum class SHADER_OUT_RES
			{
				DIR_LIGHT_LI,
				COUNT
			};

			ReSTIR() noexcept;
			~ReSTIR() noexcept;

			void Init() noexcept;
			bool IsInitialized() noexcept { return m_psos[0] != nullptr; };
			void Reset() noexcept;
			void OnGBufferResized() noexcept;
			void SetDescriptor(int i, uint32_t heapIdx) noexcept
			{
				Assert(i < (int)SHADER_IN_DESC::COUNT, "out-of-bound access.");
				m_inputDesc[i] = heapIdx;
			}

			const Texture& GetOutput(int i) const noexcept
			{
				Assert(i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
				return m_outputColor;
			}

			void Render(CommandList& cmdList) noexcept;

		private:
			void CreateSizeDependantResources() noexcept;
			void CreateSizeIndependantResources() noexcept;
			void InitParams() noexcept;

			enum class SHADERS
			{
				TEMPORAL_FILTER,
				SPATIAL_FILTER,
				COUNT
			};

			static constexpr int NUM_CBV = 2;
			static constexpr int NUM_SRV = 5;
			static constexpr int NUM_UAV = 1;
			static constexpr int NUM_GLOBS = 1;
			static constexpr int NUM_CONSTS = 0;

			inline static RpObjects s_rpObjs;

			inline static const char* COMPILED_CS[(int)SHADERS::COUNT] =
			{
				"ReSTIR_TemporalPass_cs.cso",
				"ReSTIR_SpatialPass_cs.cso"
			};

			struct DefaultParamVals
			{
				static constexpr uint32_t NumRISCandidates = 32;
				static constexpr float MaxMScale = 20;
				static constexpr float NormalAngleThresh = 0.906307f;
				static constexpr float DepthToleranceScale = 1.1f;
				static constexpr float TemporalSampleBiasThresh = 1e-4f;
				static constexpr uint32_t NumSpatialSamples = 1;
				static constexpr uint32_t NumSpatialSamplesDisocclusion = 2;
				static constexpr float SpatialNeighborSearchRadius = 30.0f;
			};

			// both shaders use the same root signature
			RootSignature m_rootSig;
			ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };

			// desc. heap indices for input resources
			uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT];

			// ping-pong between frames
			DefaultHeapBuffer m_reservoirs[2];

			// color output
			Texture m_outputColor;
			DescriptorTable m_descTable;	// UAV

			// local constant buffer
			UploadHeapBuffer m_localCB;
			cbReSTIR m_cbReSTIR;

			// halton sequence
			static const int k_haltonSeqLength = 64;
			DefaultHeapBuffer m_halton;

			// parameter callbacks
			void NumRISCandidatesCallback(const ParamVariant& p) noexcept;
			void MaxMScaleCallback(const ParamVariant& p) noexcept;
			void NormalAngleThreshCallback(const ParamVariant& p) noexcept;
			void DepthToleranceScaleCallback(const ParamVariant& p) noexcept;
			void TemporalSampleBiasThreshCallback(const ParamVariant& p) noexcept;
			void NumSpatialSamplesCallback(const ParamVariant& p) noexcept;
			void NumSpatialSamplesDisocclusionCallback(const ParamVariant& p) noexcept;
			void SpatialNeighborSearchRadiusCallback(const ParamVariant& p) noexcept;
		};
	}
}