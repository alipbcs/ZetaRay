#pragma once

#include "../RenderPass.h"
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
    struct GuiPass final : public RenderPassBase<1>
    {
        enum SHADER_IN_CPU_DESC
        {
            RTV,
            COUNT
        };

        GuiPass();
        ~GuiPass() = default;

        void Init();
        void SetCPUDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h)
        {
            Assert(i < SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
            m_cpuDescriptors[i] = h;
        }
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

        static constexpr int NUM_CBV = 0;
        static constexpr int NUM_SRV = 0;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 0;
        static constexpr int NUM_CONSTS = sizeof(cbGuiPass) / sizeof(DWORD);

        inline static constexpr const char* COMPILED_VS[] = { "ImGui_vs.cso" };
        inline static constexpr const char* COMPILED_PS[] = { "ImGui_ps.cso" };

        struct ImGuiFrameBufferData
        {
            Core::GpuMemory::UploadHeapBuffer IndexBuffer;
            Core::GpuMemory::UploadHeapBuffer VertexBuffer;
            int NumIndices = 10000;
            int NumVertices = 5000;
        };

        ImGuiFrameBufferData m_imguiFrameBuffs[Core::Constants::NUM_BACK_BUFFERS];
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescriptors[SHADER_IN_CPU_DESC::COUNT] = { 0 };
        Util::SmallVector<Core::GpuTimer::Timing> m_cachedTimings;

        int m_currShader = -1;
        static constexpr float m_dbgWndWidthPct = 0.21f;
        static constexpr float m_dbgWndHeightPct = 1.0f;
        float m_logWndWidth = 0.0f;
        static constexpr float m_logWndHeightPct = 0.21f;
        static constexpr float m_headerWndHeightPct = 0.02f;
        bool m_firstTime = true;
        bool m_closeLogsTab = false;
        int m_prevNumLogs = 0;

        Util::SmallVector<App::LogMessage> m_logs;
    };
}
