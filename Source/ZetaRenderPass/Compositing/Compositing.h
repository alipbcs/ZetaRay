#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "Compositing_Common.h"

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
    enum class COMPOSITING_SHADER
    {
        COMPOSIT,
        FIREFLY_FILTER,
        COUNT
    };

    struct Compositing final : public RenderPassBase<(int)COMPOSITING_SHADER::COUNT>
    {
        enum class SHADER_IN_GPU_DESC
        {
            SKY_DI,
            INSCATTERING,
            SUN_SHADOW,
            EMISSIVE_DI,
            INDIRECT,
            COUNT
        };

        enum class SHADER_OUT_RES
        {
            COMPOSITED,
            COUNT
        };

        Compositing();
        ~Compositing() = default;

        void Init();
        void SetInscatteringEnablement(bool enable) { SET_CB_FLAG(m_cbComposit, CB_COMPOSIT_FLAGS::INSCATTERING, enable); }
        void SetSkyIllumEnablement(bool enable);
        void SetVoxelGridDepth(float zNear, float zFar) { m_cbComposit.VoxelGridNearZ = zNear, m_cbComposit.VoxelGridFarZ = zFar; }
        void SetVoxelGridMappingExp(float exp) { m_cbComposit.DepthMappingExp = exp; }
        void SetLightVoxelGridParams(const Math::uint3& dim, const Math::float3& extents, float offset_y)
        {
            m_cbComposit.GridDim_x = (uint16_t)dim.x;
            m_cbComposit.GridDim_y = (uint16_t)dim.y;
            m_cbComposit.GridDim_z = (uint16_t)dim.z;

            m_cbComposit.Extents_x = extents.x;
            m_cbComposit.Extents_y = extents.y;
            m_cbComposit.Extents_z = extents.z;

            m_cbComposit.Offset_y = offset_y;
        }
        void SetGpuDescriptor(SHADER_IN_GPU_DESC input, uint32_t descHeapIdx)
        {
            Assert((int)input < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");

            switch (input)
            {
            case SHADER_IN_GPU_DESC::INSCATTERING:
                m_cbComposit.InscatteringDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::SUN_SHADOW:
                m_cbComposit.SunShadowDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::SKY_DI:
                m_cbComposit.SkyDIDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::EMISSIVE_DI:
                m_cbComposit.EmissiveDIDescHeapIdx = descHeapIdx;
                return;
            case SHADER_IN_GPU_DESC::INDIRECT:
                m_cbComposit.IndirectDescHeapIdx = descHeapIdx;
                return;
            default:
                Assert(false, "unreachable case.");
                return;
            }
        }
        const Core::GpuMemory::Texture & GetOutput(SHADER_OUT_RES out) const
        {
            Assert((int)out < (int)SHADER_IN_GPU_DESC::COUNT, "out-of-bound access.");
            return m_compositTex;
        }
        void OnWindowResized();
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 0;
        static constexpr int NUM_UAV = 0;
        static constexpr int NUM_GLOBS = 1;
        static constexpr int NUM_CONSTS = sizeof(cbCompositing) / sizeof(DWORD);
        using SHADER = RenderPass::COMPOSITING_SHADER;

        struct ResourceFormats
        {
            static constexpr DXGI_FORMAT LIGHT_ACCUM = DXGI_FORMAT_R16G16B16A16_FLOAT;
        };

        enum class DESC_TABLE
        {
            LIGHT_ACCUM_UAV,
            COUNT
        };

        inline static constexpr const char* COMPILED_CS[(int)SHADER::COUNT] = {
            "Compositing_cs.cso",
            "FireflyFilter_cs.cso"
        };

        void CreateCompositTexure();

        // param callbacks
        void FireflyFilterCallback(const Support::ParamVariant& p);
        void DirectSunCallback(const Support::ParamVariant& p);
        void DirectSkyCallback(const Support::ParamVariant& p);
        void IndirectCallback(const Support::ParamVariant& p);
        void DirectEmissiveCallback(const Support::ParamVariant& p);
        void VisualizeLVGCallback(const Support::ParamVariant& p);
        // shader reload
        void ReloadCompsiting();

        Core::GpuMemory::Texture m_compositTex;
        Core::DescriptorTable m_descTable;
        cbCompositing m_cbComposit;
        bool m_filterFirefly = false;
    };
}