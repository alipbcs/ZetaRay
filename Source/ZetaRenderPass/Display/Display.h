#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include <Scene/SceneCommon.h>
#include "Display_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
    class GraphicsCmdList;
    struct RenderNodeHandle;
}

namespace ZetaRay::Core::GpuMemory
{
    struct ReadbackHeapBuffer;
}

namespace ZetaRay::Support
{
    struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
    enum class DISPLAY_SHADER
    {
        DISPLAY,
        DRAW_PICKED,
        DRAW_PICKED_WIREFRAME,
        SOBEL,
        COUNT
    };

    struct DisplayPass final : public RenderPassBase<(int)DISPLAY_SHADER::COUNT>
    {
        enum class SHADER_IN_CPU_DESC
        {
            RTV,
            COUNT
        };

        enum class SHADER_IN_GPU_DESC
        {
            COMPOSITED,
            EXPOSURE,
            COUNT
        };

        DisplayPass();
        ~DisplayPass() = default;

        void Init();
        void SetCpuDescriptor(SHADER_IN_CPU_DESC i, D3D12_CPU_DESCRIPTOR_HANDLE h)
        {
            Assert((int)i < (int)SHADER_IN_CPU_DESC::COUNT, "out-of-bound access.");
            m_cpuDescs[(int)i] = h;
        }
        void SetGpuDescriptor(SHADER_IN_GPU_DESC i, uint32_t dechHeapIdx)
        {
            Assert((int)i < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");
            switch (i)
            {
            case SHADER_IN_GPU_DESC::COMPOSITED:
                m_compositedSrvDescHeapIdx = dechHeapIdx;
                break;
            case SHADER_IN_GPU_DESC::EXPOSURE:
                m_cbLocal.ExposureDescHeapIdx = dechHeapIdx;
                break;
            default:
                break;
            }
        }
        void SetPickData(const Core::RenderNodeHandle& producerHandle, 
            Core::GpuMemory::ReadbackHeapBuffer* readback,
            fastdelegate::FastDelegate0<> dlg);
        void ClearPick();
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 0;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 1;
        static constexpr int NUM_CONSTS = (int)Math::Max((sizeof(cbDisplayPass) / sizeof(DWORD)),
            sizeof(cbDrawPicked) / sizeof(DWORD));

        enum DESC_TABLE
        {
            TONEMAPPER_LUT_SRV,
            PICK_MASK_SRV,
            COUNT
        };

        struct Params
        {
            inline static const char* DisplayOptions[] = { "Default", "BaseColor", "Normal",
                "Metalness-Roughness", "Coat (Weight)", "Coat (Color)", "Roughness (Threshold)", 
                "Emission", "Transmission", "Depth" };
            static_assert((int)DisplayOption::COUNT == ZetaArrayLen(DisplayOptions), "enum <-> strings mismatch.");

            inline static const char* Tonemappers[] = { "None", "Neutral", "AgX (Default)", "AgX (Golden)", 
                "AgX (Punchy)", "AgX (Custom)" };
            static_assert((int)Tonemapper::COUNT == ZetaArrayLen(Tonemappers), "enum <-> strings mismatch.");
        };

        inline static constexpr const char* COMPILED_VS[(int)DISPLAY_SHADER::COUNT] = { 
            "Display_vs.cso",
            "DrawPicked_vs.cso",
            "DrawPicked_vs.cso",
            "Sobel_vs.cso"
        };
        inline static constexpr const char* COMPILED_PS[(int)DISPLAY_SHADER::COUNT] = { 
            "Display_ps.cso" ,
            "DrawPicked_ps.cso", 
            "DrawPicked_ps.cso", 
            "Sobel_ps.cso" 
        };

        void DrawPicked(Core::GraphicsCmdList& cmdList);
        void CreatePSOs();
        void ReadbackPickIdx();

        // parameter callbacks
        void DisplayOptionCallback(const Support::ParamVariant& p);
        void TonemapperCallback(const Support::ParamVariant& p);
        void SaturationCallback(const Support::ParamVariant& p);
        void AgxExpCallback(const Support::ParamVariant& p);
        void AutoExposureCallback(const Support::ParamVariant& p);
        void RoughnessThCallback(const Support::ParamVariant& p);
        void WireframeCallback(const Support::ParamVariant& p);

        Core::GpuMemory::Texture m_lut;
        Core::DescriptorTable m_descTable;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };
        cbDisplayPass m_cbLocal;
        uint32_t m_compositedSrvDescHeapIdx = UINT32_MAX;
        // Picking data
        int m_producerHandle = -1;
        Core::GpuMemory::ReadbackHeapBuffer* m_readback = nullptr;
        std::atomic_uint64_t m_pickID = Scene::INVALID_INSTANCE;
        fastdelegate::FastDelegate0<> m_pickDlg;
        Core::GpuMemory::Texture m_pickMask;
        Core::DescriptorTable m_rtvDescTable;
        bool m_wireframe = false;
    };
}