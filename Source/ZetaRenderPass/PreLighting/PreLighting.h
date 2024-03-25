#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "PreLighting_Common.h"

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
    // Prepares for lighting by
    //  - estimating lumen for each emissive triangle (needed for power sampling light sources)
    //  - estimating local curvature (needed for ray cones)
    struct PreLighting final : public RenderPassBase
    {
        PreLighting();
        ~PreLighting();

        void Init();
        bool IsInitialized() const { return m_psos[0] != nullptr; };
        void Reset();
        void OnWindowResized();
        void SetLightPresamplingParams(uint32_t minToEnale, uint32_t numSampleSets, uint32_t sampleSetSize)
        { 
            m_minNumLightsForPresampling = minToEnale;
            m_numSampleSets = numSampleSets;
            m_sampleSetSize = sampleSetSize;
        }
        void SetLightVoxelGridParams(bool enabled, const Math::uint3& gridDim, const Math::float3& extents, float offset_y)
        {
            m_useLVG = enabled;
            m_voxelGridDim = gridDim;
            m_voxelExtents = extents;
            m_yOffset = offset_y;
        }
        const Core::GpuMemory::DefaultHeapBuffer& GetLumenBuffer() { return m_lumen; }
        const Core::GpuMemory::DefaultHeapBuffer& GePresampledSets() { return m_sampleSets; }
        const Core::GpuMemory::DefaultHeapBuffer& GetLightVoxelGrid() { return m_lvg; }
        Core::GpuMemory::ReadbackHeapBuffer& GetLumenReadbackBuffer() { return m_readback; }
        const Core::GpuMemory::Texture& GetCurvature0() { return m_curvature[0]; }
        const Core::GpuMemory::Texture& GetCurvature1() { return m_curvature[1]; }
        // Releasing the lumen buffer and its readback buffer should happen after the alias table 
        // has been calculated. Delegate that to code that does that calculation.
        auto GetReleaseBuffersDlg() { return fastdelegate::MakeDelegate(this, &PreLighting::ReleaseLumenBufferAndReadback); };

        void Update();
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 3;
        static constexpr int NUM_UAV = 1;
        static constexpr int NUM_GLOBS = 3;
        static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbPresampling) / sizeof(DWORD), 
            Math::Max(sizeof(cbLVG) / sizeof(DWORD), sizeof(cbCurvature) / sizeof(DWORD)));

        enum class SHADERS
        {
            ESTIMATE_TRIANGLE_LUMEN,
            PRESAMPLING,
            ESTIMATE_CURVATURE,
            BUILD_LIGHT_VOXEL_GRID,
            COUNT
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
            "EstimateTriLumen_cs.cso",
            "PresampleEmissives_cs.cso",
            "EstimateCurvature_cs.cso",
            "BuildLightVoxelGrid_cs.cso"
        };

        enum class DESC_TABLE
        {
            CURVATURE_0_UAV,
            CURVATURE_1_UAV,
            COUNT
        };

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT CURVATURE = DXGI_FORMAT_R16_FLOAT;
        };

        void ToggleLVG();
        void CreateOutputs();
        void ReleaseLumenBufferAndReadback();
        void ReloadBuildLVG();

        Core::GpuMemory::DefaultHeapBuffer m_halton;
        Core::GpuMemory::DefaultHeapBuffer m_lumen;
        Core::GpuMemory::ReadbackHeapBuffer m_readback;
        Core::GpuMemory::DefaultHeapBuffer m_sampleSets;
        Core::GpuMemory::DefaultHeapBuffer m_lvg;
        Core::GpuMemory::Texture m_curvature[2];
        Core::DescriptorTable m_descTable;
        ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
        uint32_t m_currNumTris = 0;
        uint32_t m_minNumLightsForPresampling = UINT32_MAX;
        uint32_t m_numSampleSets = 0;
        uint32_t m_sampleSetSize = 0;
        Math::uint3 m_voxelGridDim;
        Math::float3 m_voxelExtents;
        float m_yOffset = 0.0;
        bool m_estimateLumenThisFrame;
        bool m_doPresamplingThisFrame;
        bool m_buildLVGThisFrame = false;
        bool m_useLVG = false;
    };

    struct EmissiveTriangleAliasTable
    {
        enum class SHADER_OUT_RES
        {
            ALIAS_TABLE,
            COUNT
        };

        EmissiveTriangleAliasTable() = default;
        ~EmissiveTriangleAliasTable() = default;

        Core::GpuMemory::DefaultHeapBuffer& GetOutput(SHADER_OUT_RES i)
        {
            Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
            return m_aliasTable;
        }

        void Update(Core::GpuMemory::ReadbackHeapBuffer* readback);
        void SetEmissiveTriPassHandle(Core::RenderNodeHandle& emissiveTriHandle);
        void SetRelaseBuffersDlg(fastdelegate::FastDelegate0<> dlg) { m_releaseDlg = dlg; }
        void Render(Core::CommandList& cmdList);

    private:
        Core::GpuMemory::DefaultHeapBuffer m_aliasTable;
        Core::GpuMemory::UploadHeapBuffer m_aliasTableUpload;
        Core::GpuMemory::ReadbackHeapBuffer* m_readback = nullptr;
        fastdelegate::FastDelegate0<> m_releaseDlg;
        uint32_t m_currNumTris = 0;
        int m_emissiveTriHandle = -1;
    };
}