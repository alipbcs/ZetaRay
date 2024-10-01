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
    enum class PRE_LIGHTING_SHADER
    {
        ESTIMATE_TRIANGLE_LUMEN,
        PRESAMPLING,
        BUILD_LIGHT_VOXEL_GRID,
        COUNT
    };

    // Prepares for lighting by
    //  - estimating lumen for each emissive triangle (needed for power sampling light sources)
    //  - estimating local curvature (needed for ray cones)
    struct PreLighting final : public RenderPassBase<(int)PRE_LIGHTING_SHADER::COUNT>
    {
        PreLighting();
        ~PreLighting() = default;

        void Init();
        void OnWindowResized() {};
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
        const Core::GpuMemory::Buffer& GetLumenBuffer() { return m_lumen; }
        const Core::GpuMemory::Buffer& GePresampledSets() { return m_sampleSets; }
        const Core::GpuMemory::Buffer& GetLightVoxelGrid() { return m_lvg; }
        Core::GpuMemory::ReadbackHeapBuffer& GetLumenReadbackBuffer() { return m_readback; }
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
        using SHADER = PRE_LIGHTING_SHADER;

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "EstimateTriLumen_cs.cso",
            "PresampleEmissives_cs.cso",
            "BuildLightVoxelGrid_cs.cso"
        };

        void ToggleLVG();
        void ReleaseLumenBufferAndReadback();
        void ReloadBuildLVG();

        Core::GpuMemory::Buffer m_halton;
        Core::GpuMemory::Buffer m_lumen;
        Core::GpuMemory::ReadbackHeapBuffer m_readback;
        Core::GpuMemory::Buffer m_sampleSets;
        Core::GpuMemory::Buffer m_lvg;
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

        ZetaInline Core::GpuMemory::Buffer& GetOutput(SHADER_OUT_RES i)
        {
            Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
            return m_aliasTable;
        }
        ZetaInline void SetReleaseBuffersDlg(fastdelegate::FastDelegate0<> dlg) { m_releaseDlg = dlg; }
        ZetaInline bool HasPendingRender() { return m_fence != UINT64_MAX; }

        void Update(Core::GpuMemory::ReadbackHeapBuffer* readback);
        void SetEmissiveTriPassHandle(Core::RenderNodeHandle& emissiveTriHandle);
        void Render(Core::CommandList& cmdList);

    private:
        Core::GpuMemory::Buffer m_aliasTable;
        Core::GpuMemory::UploadHeapBuffer m_aliasTableUpload;
        Core::GpuMemory::ReadbackHeapBuffer* m_readback = nullptr;
        fastdelegate::FastDelegate0<> m_releaseDlg;
        uint32_t m_currNumTris = 0;
        int m_emissiveTriHandle = -1;
        uint64_t m_fence = UINT64_MAX;
    };
}