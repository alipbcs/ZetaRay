#pragma once

#include "../../RenderPass.h"
#include <Core/GpuMemory.h>
#include "SkyDI_Common.h"

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
    enum class SKY_DI_SHADER
    {
        SKY_DI,
        DNSR_TEMPORAL,
        DNSR_SPATIAL,
        COUNT
    };

    struct SkyDI final : public RenderPassBase<(int)SKY_DI_SHADER::COUNT>
    {
        enum class SHADER_OUT_RES
        {
            DENOISED,
            COUNT
        };

        SkyDI();
        ~SkyDI() = default;

        void Init();
        void OnWindowResized();
        const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
        {
            Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
            return m_final;
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 1;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 2;
        static constexpr int NUM_CONSTS = (int)(sizeof(cb_SkyDI) / sizeof(DWORD));
        using SHADER = SKY_DI_SHADER;

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R8_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R16G16_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R32G32_FLOAT;
            static constexpr DXGI_FORMAT COLOR_A = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT COLOR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT FINAL = DXGI_FORMAT_R16G16B16A16_FLOAT;
        };

        enum class DESC_TABLE
        {
            RESERVOIR_0_A_SRV,
            RESERVOIR_0_B_SRV,
            RESERVOIR_0_C_SRV,
            RESERVOIR_0_A_UAV,
            RESERVOIR_0_B_UAV,
            RESERVOIR_0_C_UAV,
            //
            RESERVOIR_1_A_SRV,
            RESERVOIR_1_B_SRV,
            RESERVOIR_1_C_SRV,
            RESERVOIR_1_A_UAV,
            RESERVOIR_1_B_UAV,
            RESERVOIR_1_C_UAV,
            //
            COLOR_A_SRV,
            COLOR_A_UAV,
            COLOR_B_SRV,
            COLOR_B_UAV,
            //
            DNSR_TEMPORAL_CACHE_DIFFUSE_0_SRV,
            DNSR_TEMPORAL_CACHE_SPECULAR_0_SRV,
            DNSR_TEMPORAL_CACHE_DIFFUSE_0_UAV,
            DNSR_TEMPORAL_CACHE_SPECULAR_0_UAV,
            DNSR_TEMPORAL_CACHE_DIFFUSE_1_SRV,
            DNSR_TEMPORAL_CACHE_SPECULAR_1_SRV,
            DNSR_TEMPORAL_CACHE_DIFFUSE_1_UAV,
            DNSR_TEMPORAL_CACHE_SPECULAR_1_UAV,
            FINAL_UAV,
            //
            COUNT
        };

        struct DefaultParamVals
        {
            static constexpr int M_MAX_SKY = 15;
            static constexpr int M_MAX_SUN = 3;
            // Use half-vector copy for anything lower
            static constexpr float ROUGHNESS_MIN = 0.35;
            static constexpr int DNSRTspp_Diffuse = 16;
            static constexpr int DNSRTspp_Specular = 16;
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "SkyDI_cs.cso",
            "SkyDI_DNSR_Temporal_cs.cso",
            "SkyDI_DNSR_Spatial_cs.cso"
        };

        struct Reservoir
        {
            static constexpr int NUM = 3;

            // Texture2D<uint>: (metadata)
            Core::GpuMemory::Texture A;
            // Texture2D<uint2>: wi
            Core::GpuMemory::Texture B;
            // Texture2D<float2>: (w_sum, W)
            Core::GpuMemory::Texture C;
        };

        struct DenoiserCache
        {
            Core::GpuMemory::Texture Diffuse;
            Core::GpuMemory::Texture Specular;
        };

        void CreateOutputs();

        void TemporalResamplingCallback(const Support::ParamVariant& p);
        void SpatialResamplingCallback(const Support::ParamVariant& p);
        void MaxMSkyCallback(const Support::ParamVariant& p);
        void MaxMSunCallback(const Support::ParamVariant& p);
        void AlphaMinCallback(const Support::ParamVariant& p);
        void DenoisingCallback(const Support::ParamVariant& p);
        void TsppDiffuseCallback(const Support::ParamVariant& p);
        void TsppSpecularCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p);

        // shader reload
        void ReloadTemporalPass();
        void ReloadDNSRTemporal();
        void ReloadDNSRSpatial();

        Reservoir m_reservoir[2];
        Core::GpuMemory::ResourceHeap m_resHeap;
        Core::GpuMemory::Texture m_colorA;
        Core::GpuMemory::Texture m_colorB;
        DenoiserCache m_dnsrCache[2];
        Core::GpuMemory::Texture m_final;
        int m_currTemporalIdx = 0;
        bool m_temporalResampling = true;
        bool m_spatialResampling = true;
        bool m_isTemporalReservoirValid = false;
        bool m_isDnsrTemporalCacheValid = false;

        Core::DescriptorTable m_descTable;

        cb_SkyDI m_cbSpatioTemporal;
        cb_SkyDI_DNSR_Temporal m_cbDnsrTemporal;
        cb_SkyDI_DNSR_Spatial m_cbDnsrSpatial;
    };
}