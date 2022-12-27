#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "Compositing_Common.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct Compositing final
		{
			enum class SHADER_IN_GPU_DESC
			{
				HDR_LIGHT_ACCUM,
				DENOISED_L_IND,
				INSCATTERING,
				COUNT
			};

			Compositing() noexcept;
			~Compositing() noexcept;

			void Init() noexcept;
			bool IsInitialized() noexcept { return m_pso != nullptr; }
			void Reset() noexcept;
			void SetInscatteringEnablement(bool b) { m_localCB.AccumulateInscattering = b; }
			void SetIndirectDiffuseEnablement(bool b) { m_localCB.UseDenoised = b; }
			void SetVoxelGridDepth(float zNear, float zFar) noexcept { m_localCB.VoxelGridNearZ = zNear, m_localCB.VoxelGridFarZ = zFar; }
			void SetVoxelGridMappingExp(float p) noexcept { m_localCB.DepthMappingExp = p; }
			void SetGPUDescriptor(SHADER_IN_GPU_DESC i, uint32_t descHeapIdx) noexcept
			{
				Assert((int)i < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");

				switch (i)
				{
				case SHADER_IN_GPU_DESC::HDR_LIGHT_ACCUM:
					m_localCB.HDRLightAccumDescHeapIdx = descHeapIdx;
					break;
				case SHADER_IN_GPU_DESC::DENOISED_L_IND:
					m_localCB.DenoisedLindDescHeapIdx = descHeapIdx;
					break;
				case SHADER_IN_GPU_DESC::INSCATTERING:
					m_localCB.InscatteringDescHeapIdx = descHeapIdx;
					break;
				default:
					break;
				}
			}
			void Render(CommandList& cmdList) noexcept;

		private:
			static constexpr int NUM_CBV = 1;
			static constexpr int NUM_SRV = 0;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 1;
			static constexpr int NUM_CONSTS = sizeof(cbCompositing) / sizeof(DWORD);

			inline static RpObjects s_rpObjs;

			RootSignature m_rootSig;

			inline static const char* COMPILED_CS[] = { "Compositing_cs.cso" };

			ID3D12PipelineState* m_pso = nullptr;
			//uint32_t m_gpuDescriptors[(int)SHADER_IN_GPU_DESC::COUNT] = { 0 };

			cbCompositing m_localCB{};

			// shader reload
			void ReloadShader() noexcept;
		};
	}
}