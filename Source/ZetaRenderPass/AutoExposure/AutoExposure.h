#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include "AutoExposure_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
}

namespace ZetaRay::RenderPass
{
    enum class AUTO_EXPOSURE_SHADER
    {
        HISTOGRAM,
        WEIGHTED_AVG,
        COUNT
    };

    struct AutoExposure final : public RenderPassBase<(int)AUTO_EXPOSURE_SHADER::COUNT>
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
        ~AutoExposure() = default;

        void InitPSOs();
        void Init();
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
        using SHADER = RenderPass::AUTO_EXPOSURE_SHADER;

        enum class DESC_TABLE
        {
            EXPOSURE_UAV,
            HISTOGRAM_UAV,
            COUNT
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] =
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

        void CreateResources();
        void MinLumCallback(const Support::ParamVariant& p);
        void MaxLumCallback(const Support::ParamVariant& p);
        void LumMapExpCallback(const Support::ParamVariant& p);
        void LowerPercentileCallback(const Support::ParamVariant& p);
        void UpperPercentileCallback(const Support::ParamVariant& p);
        void Reload();

        Core::GpuMemory::Texture m_exposure;
        Core::GpuMemory::Buffer m_counter;
        Core::GpuMemory::Buffer m_hist;
        Core::GpuMemory::Buffer m_zeroBuffer;        // for resetting the histogram to zero each frame
        uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT] = { 0 };
        Core::DescriptorTable m_descTable;
        float m_minLum;
        float m_maxLum;
        cbAutoExposureHist m_cbHist;
    };
}