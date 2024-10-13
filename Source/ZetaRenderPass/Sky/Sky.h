#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include "Sky_Common.h"

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
    enum class SKY_SHADER
    {
        SKY_LUT,
        INSCATTERING,
        COUNT
    };

    struct Sky final : public RenderPassBase<(int)SKY_SHADER::COUNT>
    {
        enum class SHADER_OUT_RES
        {
            SKY_VIEW_LUT,
            INSCATTERING,
            COUNT
        };

        Sky();
        ~Sky() = default;

        void Init(int lutWidth, int lutHeight, bool doInscattering);
        bool IsInscatteringEnabled() { return m_doInscattering; }
        void SetInscatteringEnablement(bool b);
        Math::uint3 GetVoxelGridDim() const
        { 
            return Math::uint3(m_localCB.NumVoxelsX, m_localCB.NumVoxelsY,
                INSCATTERING_THREAD_GROUP_SIZE_X);
        }
        Math::float2 GetVoxelGridDepth() const 
        { 
            return Math::float2(m_localCB.VoxelGridNearZ, m_localCB.VoxelGridFarZ); 
        }
        float GetVoxelGridMappingExp() const { return m_localCB.DepthMappingExp; }
        const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i) const
        {
            Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");

            if (i == SHADER_OUT_RES::SKY_VIEW_LUT)
                return m_lut;
            else
                return m_voxelGrid;
        }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 1;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 1;
        static constexpr int NUM_CONSTS = sizeof(cbSky) / sizeof(DWORD);
        using SHADER = SKY_SHADER;

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT INSCATTERING_VOXEL_GRID = DXGI_FORMAT_R11G11B10_FLOAT;
            static constexpr DXGI_FORMAT SKY_VIEW_LUT = DXGI_FORMAT_R11G11B10_FLOAT;
        };

        struct DefaultParamVals
        {
            static constexpr int NUM_VOXELS_X = 192;
            static constexpr int NUM_VOXELS_Y = int(NUM_VOXELS_X / 1.77f);
            static constexpr float DEPTH_MAP_EXP = 2.0f;
            static constexpr float VOXEL_GRID_NEAR_Z = 0.5f;
            static constexpr float VOXEL_GRID_FAR_Z = 30.0f;
        };

        enum class DESC_TABLE
        {
            SKY_LUT_UAV,
            VOXEL_GRID_UAV,
            COUNT
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "SkyViewLUT_cs.cso", "Inscattering_cs.cso" };

        void CreateSkyviewLUT();
        void CreateVoxelGrid();

        // parameter callbacks
        void DepthMapExpCallback(const Support::ParamVariant& p);
        void VoxelGridNearZCallback(const Support::ParamVariant& p);
        void VoxelGridFarZCallback(const Support::ParamVariant& p);

        // shader reload
        void ReloadInscatteringShader();
        void ReloadSkyLUTShader();

        Core::GpuMemory::Texture m_lut;
        Core::GpuMemory::Texture m_voxelGrid;
        Core::DescriptorTable m_descTable;
        cbSky m_localCB;
        bool m_doInscattering = false;
    };
}