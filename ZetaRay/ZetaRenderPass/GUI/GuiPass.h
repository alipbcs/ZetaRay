#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include <Core/GpuTimer.h>
#include <Support/MemoryArena.h>
#include "GuiPass_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct GuiPass final
	{
		enum SHADER_IN_CPU_DESC
		{
			RTV,
			COUNT
		};

		GuiPass();
		~GuiPass();

		void Init();
		bool IsInitialized() { return m_pso != nullptr; };
		void Reset();
		void SetCPUDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h)
		{
			Assert(i < SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
			m_cpuDescriptors[i] = h;
		}
		void Update();
		void Render(Core::CommandList& cmdList);

	private:
		void UpdateBuffers();
		void RenderSettings();
		void RenderProfiler();
		void RenderLogWindow();
		void RenderMainHeader();
		void InfoTab();
		void CameraTab();
		void ParameterTab();
		void GpuTimingsTab();
		void ShaderReloadTab();
		void RebuildFontTex();

		static constexpr int NUM_CBV = 0;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 0;
		static constexpr int NUM_CONSTS = sizeof(cbGuiPass) / sizeof(DWORD);

		RpObjects s_rpObjs;

		inline static constexpr const char* COMPILED_VS[] = { "ImGui_vs.cso" };
		inline static constexpr const char* COMPILED_PS[] = { "ImGui_ps.cso" };

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;

		struct ImGuiFrameBufferData
		{
			Core::GpuMemory::UploadHeapBuffer IndexBuffer;
			Core::GpuMemory::UploadHeapBuffer VertexBuffer;
			int NumIndices = 10000;
			int NumVertices = 5000;
		};

		ImGuiFrameBufferData m_imguiFrameBuffs[Core::Constants::NUM_BACK_BUFFERS];
		Core::GpuMemory::Texture m_imguiFontTex;
		Core::DescriptorTable m_fontTexSRV;
		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescriptors[SHADER_IN_CPU_DESC::COUNT] = { 0 };

		Util::SmallVector<Core::GpuTimer::Timing> m_cachedTimings;

		int m_currShader = -1;
		static constexpr float m_dbgWndWidthPct = 0.21f;
		static constexpr float m_dbgWndHeightPct = 1.0f;
		float m_logWndWidth = 0.0f;
		static constexpr float m_logWndHeightPct = 0.21f;
		static constexpr float m_headerWndHeightPct = 0.02f;
		bool m_isFullScreen = false;
		int m_prevNumLogs = 0;
		bool m_showLogsWindow = true;
		//float m_headerSpacingX = 0;

		Util::SmallVector<App::LogMessage> m_logs;

		struct FontTex
		{
			fastdelegate::FastDelegate0<> RebuildFontTexDlg;
			uint8_t* Pixels;
			int Width;
			int Height;
			bool IsStale;
		};

		FontTex m_font;
	};
}
