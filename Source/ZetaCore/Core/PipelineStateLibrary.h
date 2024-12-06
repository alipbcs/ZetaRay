#pragma once

#include "../Core/Device.h"
#include "../App/Path.h"
#include <atomic>

namespace ZetaRay::Core
{
    class PipelineStateLibrary
    {
    public:
        explicit PipelineStateLibrary(Util::MutableSpan<ID3D12PipelineState*> psoCache);
        ~PipelineStateLibrary();

        PipelineStateLibrary(PipelineStateLibrary&&) = delete;
        PipelineStateLibrary& operator=(PipelineStateLibrary&&) = delete;

        void Init(const char* name);
        void Reset();
        void Reload(uint64_t idx, ID3D12RootSignature* rootSig, const char* pathToHlsl, 
            bool flushGpu = false);

        ID3D12PipelineState* CompileGraphicsPSO(uint32_t idx,
            D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc,
            ID3D12RootSignature* rootSig,
            const char* pathToCompiledVS,
            const char* pathToCompiledPS);
        ID3D12PipelineState* CompileComputePSO(uint32_t idx,
            ID3D12RootSignature* rootSig,
            const char* pathToCompiledCS);
        ID3D12PipelineState* CompileComputePSO_MT(uint32_t idx,
            ID3D12RootSignature* rootSig,
            const char* pathToCompiledCS);
        ID3D12PipelineState* CompileComputePSO(uint32_t idx,
            ID3D12RootSignature* rootSig,
            Util::Span<const uint8_t> compiledBlob);

        ZetaInline ID3D12PipelineState* GetPSO(uint32_t idx)
        {
            return m_compiledPSOs[idx];
        }

    private:
        void ResetToEmptyPsoLib();
        void ClearAndFlushToDisk();

        App::Filesystem::Path m_psoLibPath1;
        ComPtr<ID3D12PipelineLibrary> m_psoLibrary;
        Util::MutableSpan<ID3D12PipelineState*> m_compiledPSOs;
        Util::SmallVector<uint8_t> m_cachedBlob;

        SRWLOCK m_mapLock = SRWLOCK_INIT;
        std::atomic_bool m_needsRebuild = false;
        bool m_foundOnDisk = false;
        bool m_psoWasReset = false;
    };
}