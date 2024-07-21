#pragma once

#include "../../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
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
        SPATIO_TEMPORAL,
        SPATIO_TEMPORAL_LIGHT_PRESAMPLING,
        DNSR_TEMPORAL,
        DNSR_SPATIAL,
        COUNT
    };

    struct DirectLighting final : public RenderPassBase<(int)DIRECT_SHADER::COUNT>
    {
        enum class SHADER_OUT_RES
        {
            DENOISED,
            COUNT
        };

        DirectLighting();
        ~DirectLighting() = default;

        void Init();
        void OnWindowResized();
        void SetLightPresamplingParams(bool enabled, int numSampleSets, int sampleSetSize)
        {
            Assert(!enabled || (numSampleSets > 0 && sampleSetSize > 0), "Presampling is enabled, but the number of sample sets is zero.");

            m_preSampling = enabled;
            m_cbSpatioTemporal.NumSampleSets = enabled ? (uint16_t)numSampleSets : 0;
            m_cbSpatioTemporal.SampleSetSize = enabled ? (uint16_t)sampleSetSize : 0;
        }
        const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
        {
            Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
            return m_denoised;
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 5;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 6;
        static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_ReSTIR_DI_SpatioTemporal) / sizeof(DWORD),
            Math::Max(sizeof(cb_ReSTIR_DI_DNSR_Temporal) / sizeof(DWORD), sizeof(cb_ReSTIR_DI_DNSR_Spatial) / sizeof(DWORD)));
        using SHADER = DIRECT_SHADER;

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R32G32_UINT;
            static constexpr DXGI_FORMAT COLOR_A = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT COLOR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
            COLOR_A_SRV,
            COLOR_A_UAV,
            COLOR_B_SRV,
            COLOR_B_UAV,
            //
            DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV,
            DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV,
            DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV,
            DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV,
            DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV,
            DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV,
            DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV,
            DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV,
            DNSR_FINAL_UAV,
            //
            COUNT
        };

        struct DefaultParamVals
        {
            static constexpr int M_MAX = 20;
            static constexpr int DNSR_TSPP_DIFFUSE = 16;
            static constexpr int DNSR_TSPP_SPECULAR = 16;
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "ReSTIR_DI_SpatioTemporal_cs.cso",
            "ReSTIR_DI_SpatioTemporal_WPS_cs.cso",
            "ReSTIR_DI_DNSR_Temporal_cs.cso",
            "ReSTIR_DI_DNSR_Spatial_cs.cso"
        };

        struct Reservoir
        {
            // Texture2D<uint4>: ((Li.g << 16 | Li.r), (M << 16 | Li.b), (bary.y << 16 | bary.x), W)
            Core::GpuMemory::Texture ReservoirA;
            // Texture2D<uint>: (lightIdx)
            Core::GpuMemory::Texture ReservoirB;
        };

        struct DenoiserCache
        {
            Core::GpuMemory::Texture Diffuse;
            Core::GpuMemory::Texture Specular;
        };

        void CreateOutputs();

        // param callbacks
        void TemporalResamplingCallback(const Support::ParamVariant& p);
        void SpatialResamplingCallback(const Support::ParamVariant& p);
        void MaxTemporalMCallback(const Support::ParamVariant& p);
        void ExtraSamplesDisocclusionCallback(const Support::ParamVariant& p);
        void StochasticSpatialCallback(const Support::ParamVariant& p);
        void DenoiseCallback(const Support::ParamVariant& p);
        void TsppDiffuseCallback(const Support::ParamVariant& p);
        void TsppSpecularCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p);

        // shader reload
        void ReloadSpatioTemporal();
        void ReloadDnsrTemporal();
        void ReloadDnsrSpatial();

        Core::DescriptorTable m_descTable;
        Reservoir m_temporalReservoir[2];
        Core::GpuMemory::Texture m_colorA;
        Core::GpuMemory::Texture m_colorB;
        DenoiserCache m_dnsrCache[2];
        Core::GpuMemory::Texture m_denoised;

        int m_currTemporalIdx = 0;
        bool m_isTemporalReservoirValid = false;
        bool m_isDnsrTemporalCacheValid = false;
        bool m_doTemporalResampling = true;
        bool m_doSpatialResampling = true;
        bool m_preSampling = false;

        cb_ReSTIR_DI_SpatioTemporal m_cbSpatioTemporal;
        cb_ReSTIR_DI_DNSR_Temporal m_cbDnsrTemporal;
        cb_ReSTIR_DI_DNSR_Spatial m_cbDnsrSpatial;
    };
}