#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "AutoExposure_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
}

namespace ZetaRay::RenderPass
{
    struct AutoExposure final : public RenderPassBase
    {
        enum class SHADER_IN_DESC
        {
            COMPOSITED,
            COUNT
        };

        enum class SHADER_OUT_RES
        {
            EXPOSURE,
            COUNT
        };

        AutoExposure();
        ~AutoExposure();

        void Init();
        bool IsInitialized() { return m_psos[0] != nullptr || m_psos[1] != nullptr; };
        void Reset();
        void SetDescriptor(SHADER_IN_DESC i, uint32_t heapIdx)
        {
            Assert((uint32_t)i < (uint32_t)SHADER_IN_DESC::COUNT, "out-of-bound access.");
            m_inputDesc[(uint32_t)i] = heapIdx;
        }
        Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i)
        {
            Assert((uint32_t)i < (uint32_t)SHADER_OUT_RES::COUNT, "out-of-bound access.");
            return m_exposure;
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 0;
        static constexpr int NUM_UAV = 1;
        static constexpr int NUM_GLOBS = 1;
        static constexpr int NUM_CONSTS = (int)sizeof(cbAutoExposureHist) / sizeof(DWORD);

        void CreateResources();

        Core::GpuMemory::Texture m_exposure;
        Core::GpuMemory::DefaultHeapBuffer m_counter;
        Core::GpuMemory::DefaultHeapBuffer m_hist;
        Core::GpuMemory::DefaultHeapBuffer m_zeroBuffer;        // for resetting the histogram to zero each frame
        uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT] = { 0 };

        enum class DESC_TABLE
        {
            EXPOSURE_UAV,
            HISTOGRAM_UAV,
            COUNT
        };

        Core::DescriptorTable m_descTable;

        enum class SHADERS
        {
            HISTOGRAM,
            WEIGHTED_AVG,
            COUNT
        };

        ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };
        inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] =
        {
            "AutoExposure_Histogram_cs.cso",
            "AutoExposure_WeightedAvg_cs.cso"
        };

        struct DefaultParamVals
        {
            static constexpr float MinLum = 5e-3f;
            static constexpr float MaxLum = 4.0f;
            static constexpr float LumMapExp = 0.5f;
            static constexpr float AdaptationRate = 1.0f;
            static constexpr float LowerPercentile = 0.01f;
            static constexpr float UpperPercentile = 0.9f;
        };

        float m_minLum;
        float m_maxLum;
        cbAutoExposureHist m_cbHist;
        bool m_clampLum;

        void MinLumCallback(const Support::ParamVariant& p);
        void MaxLumCallback(const Support::ParamVariant& p);
        void LumMapExpCallback(const Support::ParamVariant& p);
        void LowerPercentileCallback(const Support::ParamVariant& p);
        void UpperPercentileCallback(const Support::ParamVariant& p);

        void Reload();
    };
}