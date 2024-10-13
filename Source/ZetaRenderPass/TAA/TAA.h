#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include "TAA_Common.h"

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
    struct TAA final : public RenderPassBase<1>
    {
        enum class SHADER_IN_DESC
        {
            SIGNAL,
            COUNT
        };

        enum class SHADER_OUT_RES
        {
            OUTPUT_A,
            OUTPUT_B,
            COUNT
        };

        TAA();
        ~TAA();

        void Init();
        void Reset();
        void OnWindowResized();
        void SetDescriptor(SHADER_IN_DESC i, uint32_t heapIdx)
        {
            Assert((int)i < (int)SHADER_IN_DESC::COUNT, "out-of-bound access.");
            m_inputDesc[(int)i] = heapIdx;
        }
        Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i)
        {
            Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
            return m_antiAliased[(int)i];
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 0;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 1;
        static constexpr int NUM_CONSTS = sizeof(cbTAA) / sizeof(DWORD);

        enum class DESC_TABLE
        {
            TEX_A_SRV,
            TEX_A_UAV,
            TEX_B_SRV,
            TEX_B_UAV,
            COUNT
        };

        inline static constexpr const char* COMPILED_CS[] = { "TAA_cs.cso" };

        struct DefaultParamVals
        {
            static constexpr float BlendWeight = 0.1f;
        };

        void CreateResources();
        void BlendWeightCallback(const Support::ParamVariant& p);
        void ReloadShader();

        // ping-pong between input & output
        Core::GpuMemory::Texture m_antiAliased[2];
        uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT];
        cbTAA m_localCB;
        Core::DescriptorTable m_descTable;
        bool m_isTemporalTexValid;
    };
}