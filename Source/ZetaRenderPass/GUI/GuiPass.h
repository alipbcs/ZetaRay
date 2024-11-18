#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/GpuTimer.h>
#include "GuiPass_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
}

namespace ZetaRay::Model
{
    struct TriangleMesh;
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
        void OnWindowResized();
        void SetCPUDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h)
        {
            Assert(i < SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
            m_cpuDescriptors[i] = h;
        }
        void Render(Core::CommandList& cmdList);

    private:
        void UpdateBuffers();
        void RenderUI();
        void RenderSettings(uint64 pickedID, const Model::TriangleMesh& mesh, 
            const Math::float4x4a& W);
        void RenderProfiler();
        void RenderLogWindow();
        void RenderMainHeader();
        void RenderToolbar(uint64 pickedID);
        void RenderGizmo(uint64_t pickedID, const Model::TriangleMesh& mesh, 
            const Math::float4x4a& W);
        void InfoTab();
        void CameraTab();
        void ParameterTab();
        void GpuTimingsTab();
        void ShaderReloadTab();
        void PickedMaterial(uint64 pickedID);
        void PickedWorldTransform(uint64 pickedID, const Model::TriangleMesh& mesh, 
            const Math::float4x4a& W);

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
        float m_dbgWndWidthPct = 0.21f;
        float m_dbgWndHeightPct = 1.0f;
        int m_headerWndWidth = 0;
        int m_headerWndHeight = 0;
        float m_logWndHeightPct = 0.21f;
        static constexpr float m_headerWndHeightPct = 0.02f;
        static constexpr float m_frameHistWidthPct = 0.9f;
        bool m_firstTime = true;
        bool m_logsTabOpen = true;
        bool m_manuallyCloseLogsTab = false;
        bool m_pendingEmissiveUpdate = false;
        bool m_appWndSizeChanged = false;
        bool m_hideUI = false;
        int m_prevNumLogs = 0;

        enum class ROTATION_MODE
        {
            AXIS_ANGLE,
            QUATERNION
        };

        enum class TRANSFORMATION
        {
            LOCAL,
            WORLD
        };

        ROTATION_MODE m_rotationMode = ROTATION_MODE::AXIS_ANGLE;
        TRANSFORMATION m_transform = TRANSFORMATION::LOCAL;

        // ImGuizmo::TRANSLATE
        uint32_t m_currGizmoOperation = 7;
        bool m_gizmoActive = false;
        //ImGuizmo::MODE m_currGizmoMode = ImGuizmo::WORLD;
    };
}
