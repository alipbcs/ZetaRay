#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct SkyDome final
		{
			enum SHADER_IN_DESC
			{
				RTV,
				DEPTH_BUFFER,
				COUNT
			};

			SkyDome() noexcept;
			~SkyDome() noexcept;

			void Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc) noexcept;
			bool IsInitialized() noexcept { return m_pso != nullptr; }
			void Reset() noexcept;
			void SetDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
			{
				Assert(i < SHADER_IN_DESC::COUNT, "out-of-bound access.");
				m_descriptors[i] = h;
			}
			void Render(CommandList& cmdList) noexcept;

		private:
			void ReloadShaders() noexcept;

			static constexpr int NUM_CBV = 1;
			static constexpr int NUM_SRV = 0;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 1;
			static constexpr int NUM_CONSTS = 0;

			inline static RpObjects s_rpObjs;

			RootSignature m_rootSig;

			inline static const char* COMPILED_VS[] = { "SkyDome_vs.cso" };
			inline static const char* COMPILED_PS[] = { "SkyDome_ps.cso" };

			ID3D12PipelineState* m_pso = nullptr;

			DefaultHeapBuffer m_domeVertexBuffer;
			DefaultHeapBuffer m_domeIndexBuffer;

			D3D12_VERTEX_BUFFER_VIEW m_vbv;
			D3D12_INDEX_BUFFER_VIEW m_ibv;

			D3D12_CPU_DESCRIPTOR_HANDLE m_descriptors[SHADER_IN_DESC::COUNT] = { 0 };

			D3D12_GRAPHICS_PIPELINE_STATE_DESC m_cachedPsoDesc;
		};
	}
}