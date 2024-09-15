#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include "GBufferRT_Common.h"

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
    enum class GBUFFER_SHADER
    {
        GBUFFER,
        COUNT
    };

    struct GBufferRT : public RenderPassBase<(int)GBUFFER_SHADER::COUNT>
    {
        GBufferRT();
        ~GBufferRT() = default;

        GBufferRT(GBufferRT&&) = delete;
        GBufferRT& operator=(GBufferRT&&) = delete;

        void Init();
        void SetGBufferUavDescTableGpuHeapIdx(uint32_t descHeapIdx) { m_cbLocal.UavTableDescHeapIdx = descHeapIdx; }
        ZetaInline void PickPixel(uint16 pixelX, uint16 pixelY)
        {
            m_cbLocal.PickedPixelX = pixelX;
            m_cbLocal.PickedPixelY = pixelY;
        }
        ZetaInline bool HasPendingPick() const { return m_cbLocal.PickedPixelX != UINT16_MAX; }
        ZetaInline void ClearPick()
        {
            m_cbLocal.PickedPixelX = UINT16_MAX;
        }
        Core::GpuMemory::ReadbackHeapBuffer& GetPickReadbackBuffer() { return m_readbackBuffer; }
        void Render(Core::CommandList& cmdList);

    private:
        static constexpr int NUM_CBV = 1;
        static constexpr int NUM_SRV = 5;
        static constexpr int NUM_UAV = 1;
        static constexpr int NUM_GLOBS = 6;
        static constexpr int NUM_CONSTS = (int)(sizeof(cbGBufferRt) / sizeof(DWORD));

        inline static constexpr const char* COMPILED_RTPSO = "GBufferRT_lib.cso";

        struct ShaderTable
        {
            static constexpr int NUM_RAYGEN_SHADERS = 1;
            static constexpr int NUM_MISS_SHADERS = 1;
            static constexpr int NUM_HIT_GROUPS = 1;

            Core::GpuMemory::Buffer ShaderRecords;
            void* RayGenShaderIdentifier;
            void* MissShaderIdentifier;
            void* HitGroupIdentifier;
            size_t RayGenRecordStartInBytes;
            size_t MissRecordStartInBytes;
            size_t HitRecordStartInBytes;
        };

        inline static constexpr const char* COMPILED_CS[(int)GBUFFER_SHADER::COUNT] = {
            "GBufferRT_Inline_cs.cso"
        };

        //void CreateRTPSO();
        //void BuildShaderTable();
        void ReloadGBufferInline();

        Core::GpuMemory::Buffer m_pickedInstance;
        Core::GpuMemory::ReadbackHeapBuffer m_readbackBuffer;
        //ComPtr<ID3D12StateObject> m_rtPSO;
        //ShaderTable m_shaderTable;
        cbGBufferRt m_cbLocal;
    };
}