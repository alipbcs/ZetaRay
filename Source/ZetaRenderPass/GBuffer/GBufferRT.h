#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include "GBufferRT_Common.h"

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
    enum class GBUFFER_SHADER
    {
        GBUFFER,
        COUNT
    };

    struct GBufferRT : public RenderPassBase<(int)GBUFFER_SHADER::COUNT>
    {
        enum class SHADER_IN_GPU_DESC
        {
            BASE_COLOR_UAV,
            NORMAL_UAV,
            METALLIC_ROUGHNESS_UAV,
            MOTION_VECTOR_UAV,
            EMISSIVE_COLOR_UAV,
            TRANSMISSION_UAV,
            DEPTH_UAV,
            TRI_DIFF_GEO_A_UAV,
            TRI_DIFF_GEO_B_UAV,
            COUNT
        };

        GBufferRT();
        ~GBufferRT() = default;

        GBufferRT(GBufferRT&&) = delete;
        GBufferRT& operator=(GBufferRT&&) = delete;

        void Init();
        void SetGpuDescriptor(SHADER_IN_GPU_DESC input, uint32_t descHeapIdx)
        {
            Assert((int)input < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");

            switch (input)
            {
            case SHADER_IN_GPU_DESC::BASE_COLOR_UAV:
                m_cbLocal.BaseColorUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::NORMAL_UAV:
                m_cbLocal.NormalUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::METALLIC_ROUGHNESS_UAV:
                m_cbLocal.MetallicRoughnessUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::EMISSIVE_COLOR_UAV:
                m_cbLocal.EmissiveColorUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::MOTION_VECTOR_UAV:
                m_cbLocal.MotionVectorUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::TRANSMISSION_UAV:
                m_cbLocal.TransmissionUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::DEPTH_UAV:
                m_cbLocal.DepthUavDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::TRI_DIFF_GEO_A_UAV:
                m_cbLocal.TriGeoADescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::TRI_DIFF_GEO_B_UAV:
                m_cbLocal.TriGeoBDescHeapIdx = descHeapIdx;
                return;
            default:
                Assert(false, "unreachable case.");
                return;
            }
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 5;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 6;
        static constexpr int NUM_CONSTS = (int)(sizeof(cbGBufferRt) / sizeof(DWORD));

        inline static constexpr const char* COMPILED_RTPSO = "GBufferRT_lib.cso";

        struct ShaderTable
        {
            static constexpr int NUM_RAYGEN_SHADERS = 1;
            static constexpr int NUM_MISS_SHADERS = 1;
            static constexpr int NUM_HIT_GROUPS = 1;

            Core::GpuMemory::DefaultHeapBuffer ShaderRecords;
            void* RayGenShaderIdentifier;
            void* MissShaderIdentifier;
            void* HitGroupIdentifier;
            size_t RayGenRecordStartInBytes;
            size_t MissRecordStartInBytes;
            size_t HitRecordStartInBytes;
        };

        inline static constexpr const char* COMPILED_CS[(int)GBUFFER_SHADER::COUNT] = {
            "GBufferRT_Inline_cs.cso"
        };

        void CreateRTPSO();
        void BuildShaderTable();

        // params
        //void MipmapSelectionCallback(const Support::ParamVariant& p);
        //void InlineTracingCallback(const Support::ParamVariant& p);

        // shader reload
        void ReloadGBufferInline();

        ComPtr<ID3D12StateObject> m_rtPSO;
        ShaderTable m_shaderTable;
        cbGBufferRt m_cbLocal;
    };
}