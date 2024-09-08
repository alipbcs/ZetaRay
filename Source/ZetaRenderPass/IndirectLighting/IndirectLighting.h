#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "IndirectLighting_Common.h"

namespace ZetaRay::Core
{
    class CommandList;
    class ComputeCmdList;
}

namespace ZetaRay::Support
{
    struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
    enum class INDIRECT_SHADER
    {
        PATH_TRACER,
        PATH_TRACER_WoPS,
        PATH_TRACER_WPS,
        ReSTIR_GI,
        ReSTIR_GI_WoPS,
        ReSTIR_GI_WPS,
        ReSTIR_GI_LVG,
        ReSTIR_PT_PATH_TRACE,
        ReSTIR_PT_PATH_TRACE_WoPS,
        ReSTIR_PT_PATH_TRACE_WPS,
        ReSTIR_PT_SORT_CtT,
        ReSTIR_PT_SORT_TtC,
        ReSTIR_PT_SORT_CtS,
        ReSTIR_PT_SORT_StC,
        ReSTIR_PT_REPLAY_CtT,
        ReSTIR_PT_REPLAY_CtT_E,
        ReSTIR_PT_REPLAY_TtC,
        ReSTIR_PT_REPLAY_TtC_E,
        ReSTIR_PT_REPLAY_CtS,
        ReSTIR_PT_REPLAY_CtS_E,
        ReSTIR_PT_REPLAY_StC,
        ReSTIR_PT_REPLAY_StC_E,
        ReSTIR_PT_RECONNECT_CtT,
        ReSTIR_PT_RECONNECT_CtT_E,
        ReSTIR_PT_RECONNECT_TtC,
        ReSTIR_PT_RECONNECT_TtC_E,
        ReSTIR_PT_RECONNECT_CtS,
        ReSTIR_PT_RECONNECT_CtS_E,
        ReSTIR_PT_RECONNECT_StC,
        ReSTIR_PT_RECONNECT_StC_E,
        ReSTIR_PT_SPATIAL_SEARCH,
        DNSR_TEMPORAL,
        DNSR_SPATIAL,
        COUNT
    };

    struct IndirectLighting final : public RenderPassBase<(int)INDIRECT_SHADER::COUNT>
    {
        enum class SHADER_OUT_RES
        {
            DENOISED,
            COUNT
        };

        enum class INTEGRATOR : uint8_t
        {
            PATH_TRACING,
            ReSTIR_GI,
            ReSTIR_PT,
            COUNT
        };

        IndirectLighting();
        ~IndirectLighting() = default;

        void Init(INTEGRATOR method);
        void OnWindowResized();
        void SetMethod(INTEGRATOR method);
        void SetLightPresamplingParams(bool enable, int numSampleSets, int sampleSetSize)
        {
            Assert(!enable || (numSampleSets > 0 && sampleSetSize > 0), "Presampling is enabled, but the number of sample sets is zero.");

            m_preSampling = enable;
            m_cbRGI.SampleSetSize_NumSampleSets = enable ? (numSampleSets << 16) | sampleSetSize : 0;
            m_cbRPT_PathTrace.SampleSetSize_NumSampleSets = m_cbRGI.SampleSetSize_NumSampleSets;
        }
        void SetLightVoxelGridParams(bool enabled, const Math::uint3& dim, 
            const Math::float3& extents, float offset_y)
        {
            Assert(!enabled || (dim.x > 0 && dim.y > 0 && dim.z > 0
                && extents.x > 0 && extents.y > 0 && extents.z > 0), "LVG is enabled, but the dimension is invalid.");
            Assert(!enabled || m_preSampling, "LVG can't be used while light presampling is disabled.");

            m_useLVG = enabled;
            m_cbRGI.GridDim_xy = (dim.y << 16) | dim.x;
            m_cbRGI.GridDim_z = dim.z;

            Math::half4 extH(extents.x, extents.y, extents.z, offset_y);
            m_cbRGI.Extents_xy = (extH.y << 16) | extH.x;
            m_cbRGI.Extents_z_Offset_y = (extH.w << 16) | extH.z;
        }
        const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
        {
            Assert(i == SHADER_OUT_RES::DENOISED, "Invalid shader output.");
            return m_final;
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 9;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 10;
        static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cb_ReSTIR_GI) / sizeof(DWORD),
            Math::Max(sizeof(cb_ReSTIR_PT_PathTrace) / sizeof(DWORD),
                      sizeof(cb_ReSTIR_PT_Reuse) / sizeof(DWORD)));
        using SHADER = RenderPass::INDIRECT_SHADER;

        struct ResourceFormats_RGI
        {
            static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R32G32B32A32_FLOAT;
            static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R32G32B32A32_FLOAT;
            static constexpr DXGI_FORMAT COLOR_A = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT COLOR_B = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT DNSR_TEMPORAL_CACHE = DXGI_FORMAT_R16G16B16A16_FLOAT;
        };

        struct ResourceFormats_RPT
        {
            static constexpr DXGI_FORMAT RESERVOIR_A = DXGI_FORMAT_R8G8B8A8_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_B = DXGI_FORMAT_R32G32_FLOAT;
            static constexpr DXGI_FORMAT RESERVOIR_C = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_D = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT RESERVOIR_E = DXGI_FORMAT_R16_FLOAT;
            static constexpr DXGI_FORMAT RESERVOIR_F = DXGI_FORMAT_R32G32_FLOAT;
            static constexpr DXGI_FORMAT RESERVOIR_G = DXGI_FORMAT_R32_UINT;
            static constexpr DXGI_FORMAT SPATIAL_NEIGHBOR = DXGI_FORMAT_R8G8_UINT;
            static constexpr DXGI_FORMAT THREAD_MAP = DXGI_FORMAT_R16_UINT;
            static constexpr DXGI_FORMAT RBUFFER_A = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT RBUFFER_B = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT RBUFFER_C = DXGI_FORMAT_R32G32B32A32_UINT;
            static constexpr DXGI_FORMAT TARGET = DXGI_FORMAT_R16G16B16A16_FLOAT;
            static constexpr DXGI_FORMAT FINAL = DXGI_FORMAT_R16G16B16A16_FLOAT;
        };

        enum class DESC_TABLE_RGI
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
            DNSR_FINAL_UAV,
            //
            COUNT
        };

        enum class DESC_TABLE_RPT
        {
            RESERVOIR_0_A_SRV,
            RESERVOIR_0_B_SRV,
            RESERVOIR_0_C_SRV,
            RESERVOIR_0_D_SRV,
            RESERVOIR_0_E_SRV,
            RESERVOIR_0_F_SRV,
            RESERVOIR_0_G_SRV,
            RESERVOIR_0_A_UAV,
            RESERVOIR_0_B_UAV,
            RESERVOIR_0_C_UAV,
            RESERVOIR_0_D_UAV,
            RESERVOIR_0_E_UAV,
            RESERVOIR_0_F_UAV,
            RESERVOIR_0_G_UAV,
            //
            RESERVOIR_1_A_SRV,
            RESERVOIR_1_B_SRV,
            RESERVOIR_1_C_SRV,
            RESERVOIR_1_D_SRV,
            RESERVOIR_1_E_SRV,
            RESERVOIR_1_F_SRV,
            RESERVOIR_1_G_SRV,
            RESERVOIR_1_A_UAV,
            RESERVOIR_1_B_UAV,
            RESERVOIR_1_C_UAV,
            RESERVOIR_1_D_UAV,
            RESERVOIR_1_E_UAV,
            RESERVOIR_1_F_UAV,
            RESERVOIR_1_G_UAV,
            //
            RBUFFER_A_CtN_SRV,
            RBUFFER_B_CtN_SRV,
            RBUFFER_C_CtN_SRV,
            RBUFFER_A_CtN_UAV,
            RBUFFER_B_CtN_UAV,
            RBUFFER_C_CtN_UAV,
            RBUFFER_A_NtC_SRV,
            RBUFFER_B_NtC_SRV,
            RBUFFER_C_NtC_SRV,
            RBUFFER_A_NtC_UAV,
            RBUFFER_B_NtC_UAV,
            RBUFFER_C_NtC_UAV,
            //
            THREAD_MAP_CtN_SRV,
            THREAD_MAP_CtN_UAV,
            THREAD_MAP_NtC_SRV,
            THREAD_MAP_NtC_UAV,
            //
            SPATIAL_NEIGHBOR_SRV,
            SPATIAL_NEIGHBOR_UAV,
            //
            TARGET_UAV,
            //
            FINAL_UAV,
            //
            COUNT
        };

        struct DefaultParamVals
        {
            static constexpr int M_MAX = 10;
            static constexpr int M_MAX_SPATIAL = 6;
            static constexpr int DNSR_TSPP_DIFFUSE = 32;
            static constexpr int DNSR_TSPP_SPECULAR = 20;
            static constexpr int MAX_DIFFUSE_BOUNCES = 3;
            static constexpr int MAX_GLOSSY_BOUNCES_NON_TRANSMISSIVE = 2;
            static constexpr int MAX_GLOSSY_BOUNCES_TRANSMISSIVE = 4;
            static constexpr bool RUSSIAN_ROULETTE = true;
            static constexpr bool STOCHASTIC_MULTI_BOUNCE = true;
            static constexpr bool BOILING_SUPPRESSION = false;
            static constexpr bool PATH_REGULARIZATION = false;
            static constexpr float ROUGHNESS_MIN = 0.175f;
            static constexpr float D_MIN = 1e-4f;
            static constexpr TEXTURE_FILTER TEX_FILTER = TEXTURE_FILTER::ANISOTROPIC_4X;
        };

        struct Params
        {
            inline static const char* DebugView[] = { "None", "K", "Case",
                "Found Connection", "Connection Lobe (k - 1)", "Connection Lobe (k)"};
            static_assert((int)RPT_DEBUG_VIEW::COUNT == ZetaArrayLen(DebugView), "enum <-> strings mismatch.");

            inline static const char* TextureFilter[] = { "Mip 0", "Tri-linear", "Anisotropic (4x)", "Anisotropic (16x)" };
            static_assert((int)TEXTURE_FILTER::COUNT == ZetaArrayLen(TextureFilter), "enum <-> strings mismatch.");
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "PathTracer_cs.cso",
            "PathTracer_WoPS_cs.cso",
            "PathTracer_WPS_cs.cso",
            "ReSTIR_GI_cs.cso",
            "ReSTIR_GI_WoPS_cs.cso",
            "ReSTIR_GI_WPS_cs.cso",
            "ReSTIR_GI_LVG_cs.cso",
            "ReSTIR_PT_PathTrace_cs.cso",
            "ReSTIR_PT_PathTrace_WoPS_cs.cso",
            "ReSTIR_PT_PathTrace_WPS_cs.cso",
            "ReSTIR_PT_Sort_cs.cso",
            "ReSTIR_PT_Sort_TtC_cs.cso",
            "ReSTIR_PT_Sort_CtS_cs.cso",
            "ReSTIR_PT_Sort_StC_cs.cso",
            "ReSTIR_PT_Replay_cs.cso",
            "ReSTIR_PT_Replay_E_cs.cso",
            "ReSTIR_PT_Replay_TtC_cs.cso",
            "ReSTIR_PT_Replay_TtC_E_cs.cso",
            "ReSTIR_PT_Replay_CtS_cs.cso",
            "ReSTIR_PT_Replay_CtS_E_cs.cso",
            "ReSTIR_PT_Replay_StC_cs.cso",
            "ReSTIR_PT_Replay_StC_E_cs.cso",
            "ReSTIR_PT_Reconnect_CtT_cs.cso",
            "ReSTIR_PT_Reconnect_CtT_E_cs.cso",
            "ReSTIR_PT_Reconnect_TtC_cs.cso",
            "ReSTIR_PT_Reconnect_TtC_E_cs.cso",
            "ReSTIR_PT_Reconnect_CtS_cs.cso",
            "ReSTIR_PT_Reconnect_CtS_E_cs.cso",
            "ReSTIR_PT_Reconnect_StC_cs.cso",
            "ReSTIR_PT_Reconnect_StC_E_cs.cso",
            "ReSTIR_PT_SpatialSearch_cs.cso",
            "IndirectDnsr_Temporal_cs.cso",
            "IndirectDnsr_Spatial_cs.cso"
        };

        struct Reservoir_RGI
        {
            static constexpr int NUM = 3;

            // Texture2D<float4>: (pos, ID)
            Core::GpuMemory::Texture A;
            // Texture2D<half4>: (Lo, M)
            Core::GpuMemory::Texture B;
            // Texture2D<uint4>: (w_sum, W, normal)
            Core::GpuMemory::Texture C;
        };

        struct Reservoir_RPT
        {
            static constexpr int NUM = 7;

            // Texture2D<uint8_t2>: (metadata)
            Core::GpuMemory::Texture A;
            // Texture2D<float2>: (w_sum, W)
            Core::GpuMemory::Texture B;
            // Texture2D<uint4>: (jacobian, seed_replay, ID, x_k.x)
            Core::GpuMemory::Texture C;
            // Texture2D<uint4>: (x_k.yz, w_k, L.rg | seed_nee)
            Core::GpuMemory::Texture D;
            // Texture2D<half>: (L.b)
            Core::GpuMemory::Texture E;
            // Texture2D<float2>: (lightPdf, lightNormal/dwdA)
            Core::GpuMemory::Texture F;
            // Texture2D<uint>: (seed_nee)
            Core::GpuMemory::Texture G;

            D3D12_BARRIER_LAYOUT Layout;
        };

        enum class SHIFT
        {
            CtN = 0,
            NtC,
            COUNT
        };

        struct RBuffer
        {
            static constexpr int NUM = 3;

            Core::GpuMemory::Texture A;
            Core::GpuMemory::Texture B;
            Core::GpuMemory::Texture C;
        };

        struct DenoiserCache
        {
            Core::GpuMemory::Texture Diffuse;
            Core::GpuMemory::Texture Specular;
        };

        void ResetIntegrator(bool resetAllResources, bool skipNonResources);
        void SwitchToReSTIR_PT(bool skipNonResources);
        void ReleaseReSTIR_PT();
        void SwitchToReSTIR_GI(bool skipNonResources);
        void ReleaseReSTIR_GI();
        void SwitchToPathTracer(bool skipNonResources);
        void RenderPathTracer(Core::ComputeCmdList& computeCmdList);
        void RenderReSTIR_GI(Core::ComputeCmdList& computeCmdList);
        void RenderReSTIR_PT(Core::ComputeCmdList& computeCmdList);
        void ReSTIR_PT_Temporal(Core::ComputeCmdList& computeCmdList, Util::Span<ID3D12Resource*> currReservoirs);
        void ReSTIR_PT_Spatial(Core::ComputeCmdList& computeCmdList, Util::Span<ID3D12Resource*> currReservoirs,
            Util::Span<ID3D12Resource*> prevReservoirs);

        // param callbacks
        void MaxDiffuseBouncesCallback(const Support::ParamVariant& p);
        void MaxGlossyBouncesCallback(const Support::ParamVariant& p);
        void MaxTransmissionBouncesCallback(const Support::ParamVariant& p);
        void StochasticMultibounceCallback(const Support::ParamVariant& p);
        void RussianRouletteCallback(const Support::ParamVariant& p);
        void TemporalResamplingCallback(const Support::ParamVariant& p);
        void SpatialResamplingCallback(const Support::ParamVariant& p);
        void M_maxTCallback(const Support::ParamVariant& p);
        void M_maxSCallback(const Support::ParamVariant& p);
        void BoilingSuppressionCallback(const Support::ParamVariant& p);
        void PathRegularizationCallback(const Support::ParamVariant& p);
        void AlphaMinCallback(const Support::ParamVariant& p);
        void DebugViewCallback(const Support::ParamVariant& p);
        void SortTemporalCallback(const Support::ParamVariant& p);
        void SortSpatialCallback(const Support::ParamVariant& p);
        void TexFilterCallback(const Support::ParamVariant& p);
        void DenoiseCallback(const Support::ParamVariant& p);
        void TsppDiffuseCallback(const Support::ParamVariant& p);
        void TsppSpecularCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterDiffuseCallback(const Support::ParamVariant& p);
        void DnsrSpatialFilterSpecularCallback(const Support::ParamVariant& p);

        // shader reload
        void ReloadRGI();
        void ReloadRPT_PathTrace();
        void ReloadRPT_Temporal();
        void ReloadRPT_Spatial();
        //void ReloadDnsrTemporal();
        //void ReloadDnsrSpatial();

        Core::DescriptorTable m_descTable;
        Core::GpuMemory::ResourceHeap m_resHeap;
        Reservoir_RGI m_reservoir_RGI[2];
        Reservoir_RPT m_reservoir_RPT[2];
        RBuffer m_rbuffer[(int)SHIFT::COUNT];
        Core::GpuMemory::Texture m_threadMap[(int)SHIFT::COUNT];
        Core::GpuMemory::Texture m_spatialNeighbor;
        Core::GpuMemory::Texture m_rptTarget;
        Core::GpuMemory::Texture m_colorA;
        Core::GpuMemory::Texture m_colorB;
        DenoiserCache m_dnsrCache[2];
        Core::GpuMemory::Texture m_final;

        int m_currTemporalIdx = 0;
        int m_numSpatialPasses = 0;
        bool m_isTemporalReservoirValid = false;
        bool m_isDnsrTemporalCacheValid = false;
        bool m_doTemporalResampling = true;
        bool m_preSampling = false;
        bool m_useLVG = false;
        INTEGRATOR m_method = INTEGRATOR::COUNT;

        cb_ReSTIR_GI m_cbRGI;
        cb_ReSTIR_PT_PathTrace m_cbRPT_PathTrace;
        cb_ReSTIR_PT_Reuse m_cbRPT_Reuse;
        cbIndirectDnsrTemporal m_cbDnsrTemporal;
        cbIndirectDnsrSpatial m_cbDnsrSpatial;
    };
}