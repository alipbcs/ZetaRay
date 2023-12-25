#pragma once

#include "../../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
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
    struct SkyDI final : public RenderPassBase
    {
        enum class SHADER_OUT_RES
        {
            DENOISED,
            COUNT
        };

        SkyDI();
        ~SkyDI();

        void Init();
        bool IsInitialized() const { return m_psos[0] != nullptr; };
        void Reset();
        void OnWindowResized();

        const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
        {
            Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
            return m_denoised;
        }

        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 1;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 2;
        static constexpr int NUM_CONSTS = (int)(sizeof(cb_SkyDI_Temporal) / sizeof(DWORD));

        void CreateOutputs();

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R32_FLOAT;
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
            static constexpr float MinRoughnessToResample = 0.1f;
            static constexpr int TemporalM_max = 14;
            static constexpr int DNSRTspp_Diffuse = 16;
            static constexpr int DNSRTspp_Specular = 16;
        };

        enum class SHADERS
        {
            TEMPORAL_RESAMPLE,
            DNSR_TEMPORAL,
            DNSR_SPATIAL,
            COUNT
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
            "SkyDI_SpatioTemporal_cs.cso",
            "SkyDI_DNSR_Temporal_cs.cso",
            "SkyDI_DNSR_Spatial_cs.cso"
        };

        struct Reservoir
        {
            // Texture2D<uint4>: (W, (wi.y << 16 | wi.x), (Li.g << 16 | Li.r), (M << 16 | Li.b))
            Core::GpuMemory::Texture ReservoirA;
            // Texture2D<float>: (w_sum)
            Core::GpuMemory::Texture ReservoirB;
        };

        struct DenoiserCache
        {
            Core::GpuMemory::Texture Diffuse;
            Core::GpuMemory::Texture Specular;
        };

        Reservoir m_temporalReservoir[2];
        Core::GpuMemory::Texture m_colorA;
        Core::GpuMemory::Texture m_colorB;
        DenoiserCache m_dnsrCache[2];
        Core::GpuMemory::Texture m_denoised;
        int m_currTemporalIdx = 0;
        bool m_temporalResampling = true;
        bool m_spatialResampling = true;
        bool m_isTemporalReservoirValid = false;
        bool m_isDnsrTemporalCacheValid = false;

        Core::DescriptorTable m_descTable;

        cb_SkyDI_Temporal m_cbSpatioTemporal;
        cb_SkyDI_DNSR_Temporal m_cbDnsrTemporal;
        cb_SkyDI_DNSR_Spatial m_cbDnsrSpatial;

        void TemporalResamplingCallback(const Support::ParamVariant& p);
        void SpatialResamplingCallback(const Support::ParamVariant& p);
        void MaxTemporalMCallback(const Support::ParamVariant& p);
        void MinRoughnessResampleCallback(const Support::ParamVariant& p);
        void DenoisingCallback(const Support::ParamVariant& p);
        void TsppDiffuseCallback(const Support::ParamVariant& p);
        void TsppSpecularCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p);
        //void CheckerboardingCallback(const Support::ParamVariant& p);

        ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };

        // shader reload
        void ReloadTemporalPass();
        void ReloadDNSRTemporal();
        void ReloadDNSRSpatial();
    };
}