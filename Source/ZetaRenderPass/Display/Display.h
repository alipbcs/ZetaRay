#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "Display_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
}

namespace ZetaRay::Support
{
    struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
    struct DisplayPass final : public RenderPassBase
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
        ~DisplayPass();

        void Init();
        bool IsInitialized() { return m_psosPS[0] != nullptr; }
        void Reset();
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
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 0;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 1;
        static constexpr int NUM_CONSTS = (int)(sizeof(cbDisplayPass) / sizeof(DWORD));

        enum class PS_SHADERS
        {
            DISPLAY,
            COUNT
        };

        enum DESC_TABLE
        {
            TONEMAPPER_LUT_SRV,
            COUNT
        };

        struct Params
        {
            inline static const char* DisplayOptions[] = { "Default", "BaseColor", "Normal",
                "Metalness-Roughness", "Roughness (Threshold)", "Emission", "Transmission", "Depth" };
            static_assert((int)DisplayOption::COUNT == ZetaArrayLen(DisplayOptions), "enum <-> strings mismatch.");

            inline static const char* Tonemappers[] = { "None", "Neutral", "AgX (Default)", "AgX (Golden)", "AgX (Punchy)" };
            static_assert((int)Tonemapper::COUNT == ZetaArrayLen(Tonemappers), "enum <-> strings mismatch.");
        };

        inline static constexpr const char* COMPILED_VS[(int)PS_SHADERS::COUNT] = { "Display_vs.cso" };
        inline static constexpr const char* COMPILED_PS[(int)PS_SHADERS::COUNT] = { "Display_ps.cso" };

        void CreatePSOs();
        // parameter callbacks
        void DisplayOptionCallback(const Support::ParamVariant& p);
        void TonemapperCallback(const Support::ParamVariant& p);
        void SaturationCallback(const Support::ParamVariant& p);
        void AutoExposureCallback(const Support::ParamVariant& p);
        void RoughnessThCallback(const Support::ParamVariant& p);

        ID3D12PipelineState* m_psosPS[(int)PS_SHADERS::COUNT] = { 0 };
        Core::GpuMemory::Texture m_lut;
        Core::DescriptorTable m_descTable;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuDescs[(int)SHADER_IN_CPU_DESC::COUNT] = { 0 };
        cbDisplayPass m_cbLocal;
        uint32_t m_compositedSrvDescHeapIdx = UINT32_MAX;
    };
}