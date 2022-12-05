#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "Compositing_Common.h"
#include "../../Support/Param.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct Compositing final
	{
		enum class SHADER_IN_GPU_DESC
		{
			HDR_LIGHT_ACCUM,
			DENOISED_L_IND,
			INSCATTERING,
			RESERVOIR_A,
			RESERVOIR_B,
			COUNT
		};

		Compositing() noexcept;
		~Compositing() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; }
		void Reset() noexcept;
		void SetInscatteringEnablement(bool b) { m_localCB.AccumulateInscattering = b; }
		void SetVoxelGridDepth(float zNear, float zFar) noexcept { m_localCB.VoxelGridNearZ = zNear, m_localCB.VoxelGridFarZ = zFar; }
		void SetVoxelGridMappingExp(float p) noexcept { m_localCB.DepthMappingExp = p; }
		void SetGpuDescriptor(SHADER_IN_GPU_DESC i, uint32_t descHeapIdx) noexcept
		{
			Assert((int)i < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");

			switch (i)
			{
			case SHADER_IN_GPU_DESC::HDR_LIGHT_ACCUM:
				m_localCB.HDRLightAccumDescHeapIdx = descHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::DENOISED_L_IND:
				m_localCB.DenoiserTemporalCacheDescHeapIdx = descHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::INSCATTERING:
				m_localCB.InscatteringDescHeapIdx = descHeapIdx;
				break;			
			case SHADER_IN_GPU_DESC::RESERVOIR_A:
				m_localCB.InputReservoir_A_DescHeapIdx = descHeapIdx;
				break;
			case SHADER_IN_GPU_DESC::RESERVOIR_B:
				m_localCB.InputReservoir_B_DescHeapIdx = descHeapIdx;
				break;
			default:
				break;
			}
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbCompositing) / sizeof(DWORD);

		RpObjects s_rpObjs;

		Core::RootSignature m_rootSig;

		inline static constexpr const char* COMPILED_CS[] = { "Compositing_cs.cso" };

		ID3D12PipelineState* m_pso = nullptr;
		cbCompositing m_localCB{};

		struct Params
		{
			enum Options
			{
				ALL,
				DIRECT,
				INDIRECT_DIFFUSE,
				COUNT
			};

			inline static const char* RenderOptions[] = { "Direct+Indirect", "Direct", "IndirectDiffuse" };
			static_assert(Options::COUNT == ZetaArrayLen(RenderOptions), "enum <-> strings mismatch.");
		};

		void UseRawIndirectDiffuseCallback(const Support::ParamVariant& p) noexcept;
		void ChangeLightingOptionCallback(const Support::ParamVariant& p) noexcept;

		// shader reload
		void ReloadShader() noexcept;
	};
}