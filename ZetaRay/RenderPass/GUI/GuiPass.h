#pragma once

#include "../RenderPass.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/Renderer.h"
#include "../../Core/RootSignature.h"
#include "GuiPass_Common.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct GuiPass final
		{
			enum SHADER_IN_CPU_DESC
			{
				RTV,
				DEPTH_BUFFER,
				COUNT
			};

			GuiPass() noexcept;
			~GuiPass() noexcept;

			void Init() noexcept;
			bool IsInitialized() noexcept { return m_pso != nullptr; };
			void Reset() noexcept;
			void SetCPUDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
			{
				Assert(i < SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
				m_cpuDescriptors[i] = h;
			}
			void Render(CommandList& cmdList) noexcept;

		private:
			void UpdateBuffers() noexcept;
			void RenderSettingsWindow() noexcept;
			void RenderProfilerWindow() noexcept;
			void RenderRenderGraphWindow() noexcept;
			void CameraTab() noexcept;
			void ParameterTab() noexcept;
			void GpuTimingsTab() noexcept;
			void ShaderReloadTab() noexcept;

			static constexpr int NUM_CBV = 0;
			static constexpr int NUM_SRV = 0;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 0;
			static constexpr int NUM_CONSTS = sizeof(cbGuiPass) / sizeof(DWORD);

			inline static RpObjects s_rpObjs;

			inline static const char* COMPILED_VS[] = { "ImGui_vs.cso" };
			inline static const char* COMPILED_PS[] = { "ImGui_ps.cso" };

			RootSignature m_rootSig;
			ID3D12PipelineState* m_pso = nullptr;

			struct ImGuiFrameBufferData
			{
				UploadHeapBuffer IndexBuffer;
				UploadHeapBuffer VertexBuffer;
				int NumIndices = 10000;
				int NumVertices = 5000;
			};

			ImGuiFrameBufferData m_imguiFrameBuffs[RendererConstants::NUM_BACK_BUFFERS];
			Texture m_imguiFontTex;
			DescriptorTable m_fontTexSRV;
			D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescriptors[SHADER_IN_CPU_DESC::COUNT] = { 0 };

			SmallVector<GpuTimer::Timing> m_cachedTimings;
			int m_cachedNumQueries = 0;

			int m_currShader = -1;
			bool m_isFullScreen = false;
		};
	}
}
