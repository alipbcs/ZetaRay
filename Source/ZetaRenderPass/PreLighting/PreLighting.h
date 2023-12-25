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
    // prepares for lighting by
    //  - estimating lumen for each emissive triangle (needed for power sampling light sources)
    //  - estimating local curvature (needed for virtual motion vectors and ray cones)
    struct PreLighting final : public RenderPassBase
    {
        PreLighting();
        ~PreLighting();

        void Init();
        bool IsInitialized() const { return m_psos[0] != nullptr; };
        void Reset();
        void OnWindowResized();
        constexpr int GetNumSampleSets() { return DefaultParamVals::NUM_SAMPLE_SETS; }
        constexpr int GetSampleSetSize() { return DefaultParamVals::SAMPLE_SET_SIZE; }
        bool IsPresamplingEnabled() const { return m_sampleSets.IsInitialized(); }
        const Core::GpuMemory::DefaultHeapBuffer& GetLumenBuffer() { return m_lumen; }
        const Core::GpuMemory::DefaultHeapBuffer& GePresampledSets() { return m_sampleSets; }
        const Core::GpuMemory::DefaultHeapBuffer& GetLightVoxelGrid() { return m_lvg; }
        Core::GpuMemory::ReadbackHeapBuffer& GetLumenReadbackBuffer() { return m_readback; }
        const Core::GpuMemory::Texture& GetCurvatureTexture() { return m_curvature; }
        Math::uint3 GetLightVoxelGridDim() const { return m_voxelGridDim; }
        Math::float3 GetLightVoxelGridExtents() const { return m_voxelExtents; }
        float GetLightVoxelGridYOffset() const { return m_yOffset; }
        // Releasing the lumen buffer and its readback buffer should happen after the alias table 
        // has been calculated. Delegate that to code that that does that calculation.
        auto GetReleaseBuffersDlg() { return fastdelegate::MakeDelegate(this, &PreLighting::ReleaseLumenBufferAndReadback); };

        void Update(bool emissiveLighting);
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

        struct DefaultParamVals
        {
            static constexpr int NUM_SAMPLE_SETS = 128;
            static constexpr int SAMPLE_SET_SIZE = 512;
            static constexpr float EMISSIVE_SET_MEM_BUDGET_MB = 0.5;
            static constexpr int MIN_NUM_LIGHTS_PRESAMPLING = int((EMISSIVE_SET_MEM_BUDGET_MB * 1024 * 1024) / 
                sizeof(RT::PresampledEmissiveTriangle));
            static constexpr Math::uint3 VOXEL_GRID_DIM = Math::uint3(32, 8, 40);
            static constexpr Math::float3 VOXEL_EXTENTS = Math::float3(0.6, 0.45, 0.6);
        };

        enum class DESC_TABLE
        {
            CURVATURE_UAV,
            COUNT
        };

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT CURVATURE = DXGI_FORMAT_R16_FLOAT;
        };

        void CreateOutputs();
        void ReleaseLumenBufferAndReadback();
        void ReloadBuildLVG();
        void VoxelExtentsCallback(const Support::ParamVariant& p);
        void YOffsetCallback(const Support::ParamVariant& p);

        Core::GpuMemory::DefaultHeapBuffer m_halton;
        Core::GpuMemory::DefaultHeapBuffer m_lumen;
        Core::GpuMemory::ReadbackHeapBuffer m_readback;
        Core::GpuMemory::DefaultHeapBuffer m_sampleSets;
        Core::GpuMemory::DefaultHeapBuffer m_lvg;
        Core::GpuMemory::Texture m_curvature;
        Core::DescriptorTable m_descTable;
        ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
        uint32_t m_currNumTris = 0;
        Math::uint3 m_voxelGridDim = DefaultParamVals::VOXEL_GRID_DIM;
        Math::float3 m_voxelExtents = DefaultParamVals::VOXEL_EXTENTS;
        float m_yOffset = 0.1;
        bool m_estimateLumenThisFrame;
        bool m_doPresamplingThisFrame;
        bool m_buildLVGThisFrame = false;
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