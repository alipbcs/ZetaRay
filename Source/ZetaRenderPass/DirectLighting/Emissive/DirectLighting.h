#pragma once

#include "../../RenderPass.h"
#include <Core/GpuMemory.h>
#include "DirectLighting_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
    struct RenderNodeHandle;
}

namespace ZetaRay::Support
{
    struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
    enum class DIRECT_SHADER
    {
        TEMPORAL,
        TEMPORAL_LIGHT_PRESAMPLING,
        SPATIAL,
        COUNT
    };

    struct DirectLighting final : public RenderPassBase<(int)DIRECT_SHADER::COUNT>
    {
        enum class SHADER_OUT_RES
        {
            FINAL,
            COUNT
        };

        DirectLighting();
        ~DirectLighting() = default;

        void Init();
        void OnWindowResized();
        void SetLightPresamplingParams(bool enabled, int numSampleSets, int sampleSetSize)
        {
            Assert(!enabled || (numSampleSets > 0 && sampleSetSize > 0), 
                "Presampling is enabled, but the number of sample sets is zero.");

            m_preSampling = enabled;
            m_cbSpatioTemporal.NumSampleSets = enabled ? (uint16_t)numSampleSets : 0;
            m_cbSpatioTemporal.SampleSetSize = enabled ? (uint16_t)sampleSetSize : 0;
        }
        const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
        {
            Assert(i == SHADER_OUT_RES::FINAL, "Invalid shader output.");
            return m_final;
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 6;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 7;
        static constexpr int NUM_CONSTS = (int)(sizeof(cb_ReSTIR_DI) / sizeof(DWORD));
        using SHADER = DIRECT_SHADER;

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R32G32_FLOAT;
            static constexpr DXGI_FORMAT TARGET = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT FINAL = DXGI_FORMAT_R16G16B16A16_FLOAT;
        };

        enum class DESC_TABLE
        {
            RESERVOIR_0_A_SRV,
            RESERVOIR_0_B_SRV,
            RESERVOIR_0_A_UAV,
            RESERVOIR_0_B_UAV,
            //
            RESERVOIR_1_A_SRV,
            RESERVOIR_1_B_SRV,
            RESERVOIR_1_A_UAV,
            RESERVOIR_1_B_UAV,
            //
            TARGET_UAV,
            FINAL_UAV,
            //
            COUNT
        };

        struct DefaultParamVals
        {
            static constexpr int M_MAX = 20;
            // Use half-vector copy for anything lower
            static constexpr float ROUGHNESS_MIN = 0.05;
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "ReSTIR_DI_Temporal_cs.cso",
            "ReSTIR_DI_Temporal_WPS_cs.cso",
            "ReSTIR_DI_Spatial_cs.cso"
        };

        struct Reservoir
        {
            static constexpr int NUM = 2;

            // Texture2D<uint4>: ((Li.g << 16 | Li.r), (M << 16 | Li.b), (bary.y << 16 | bary.x), W)
            Core::GpuMemory::Texture A;
            // Texture2D<uint>: (lightIdx)
            Core::GpuMemory::Texture B;

            D3D12_BARRIER_LAYOUT Layout;
        };

        void CreateOutputs();

        // param callbacks
        void TemporalResamplingCallback(const Support::ParamVariant& p);
        void SpatialResamplingCallback(const Support::ParamVariant& p);
        void MaxTemporalMCallback(const Support::ParamVariant& p);
        void ExtraSamplesDisocclusionCallback(const Support::ParamVariant& p);
        void StochasticSpatialCallback(const Support::ParamVariant& p);
        void AlphaMinCallback(const Support::ParamVariant& p);

        // shader reload
        void ReloadTemporal();

        Core::DescriptorTable m_descTable;
        Reservoir m_reservoir[2];
        Core::GpuMemory::ResourceHeap m_resHeap;
        Core::GpuMemory::Texture m_target;
        Core::GpuMemory::Texture m_final;

        int m_currTemporalIdx = 0;
        bool m_isTemporalReservoirValid = false;
        bool m_temporalResampling = true;
        bool m_spatialResampling = true;
        bool m_preSampling = false;

        cb_ReSTIR_DI m_cbSpatioTemporal;
    };
}