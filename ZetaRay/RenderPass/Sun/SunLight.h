#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct SunLight final
		{
			enum SHADER_IN_CPU_DESC
			{
				RTV,
				DEPTH_BUFFER,
				COUNT
			};

			SunLight() noexcept;
			~SunLight() noexcept;

			void Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc) noexcept;
			bool IsInitialized() noexcept { return m_pso != nullptr; };
			void Reset() noexcept;
			void SetCPUDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
			{
				Assert(i < SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
				m_cpuDescriptors[i] = h;
			}
			void Render(CommandList& cmdList) noexcept;

		private:
			void ReloadShaders() noexcept;

			static constexpr int NUM_CBV = 1;
			static constexpr int NUM_SRV = 4;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 5;
			static constexpr int NUM_CONSTS = 0;

			inline static RpObjects s_rpObjs;

			RootSignature m_rootSig;

			inline static const char* COMPILED_VS[] = { "SunLight_vs.cso" };
			inline static const char* COMPILED_PS[] = { "SunLight_ps.cso" };

			ID3D12PipelineState* m_pso = nullptr;
			D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescriptors[SHADER_IN_CPU_DESC::COUNT] = { 0 };

			D3D12_GRAPHICS_PIPELINE_STATE_DESC m_cachedPsoDesc;
		};
	}
}