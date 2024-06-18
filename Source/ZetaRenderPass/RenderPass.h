#pragma once

#include <Core/PipelineStateLibrary.h>
#include <Core/RootSignature.h>

namespace ZetaRay::RenderPass
{
    template<int nShaders>
    struct RenderPassBase
    {
    public:
        // Note: allocation may be conflated with initialization -- here, the memory for 
        // all render passes is allocated at startup and released upon shutdown. So even
        // after reset, the actual memory for the render pass object is not freed, only that
        // of resources and device objects included in the object.
        ZetaInline bool IsInitialized() const { return m_rootSigObj != nullptr; }
        void Reset(bool waitForGPU)
        {
            if (!IsInitialized())
                return;

            if (waitForGPU)
            {
                ComPtr<ID3D12Fence> m_fence;
                CheckHR(App::GetRenderer().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(m_fence.GetAddressOf())));

                App::GetRenderer().SignalDirectQueue(m_fence.Get(), 1);

                if (m_fence->GetCompletedValue() < 1)
                {
                    HANDLE handle = CreateEventA(nullptr, false, false, "");
                    CheckWin32(handle);
                    CheckHR(m_fence->SetEventOnCompletion(1, handle));

                    WaitForSingleObject(handle, INFINITE);
                    CloseHandle(handle);
                }
            }

            m_psoLib.Reset();
            uint32_t reftCount = m_rootSigObj.Reset();
            Assert(reftCount == 0, "unexpected ref count.");
        }

    protected:
        RenderPassBase(int nRootCBV, int nRootSRV, int nRootUAV, 
            int nRootGlobs, int nRootConsts)
            : m_rootSig(nRootCBV, nRootSRV, nRootUAV, nRootGlobs, nRootConsts),
            m_psoLib(m_psoCache)
        {}
        ~RenderPassBase()
        {
            if (IsInitialized())
            {
                uint32_t reftCount = m_rootSigObj.Reset();
                Assert(reftCount == 0, "unexpected ref count.");
            }
        }

        RenderPassBase(RenderPassBase&&) = delete;
        RenderPassBase& operator=(RenderPassBase&&) = delete;

        void InitRenderPass(const char* name,
            D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
            Util::Span<D3D12_STATIC_SAMPLER_DESC> samplers = Util::Span<D3D12_STATIC_SAMPLER_DESC>(nullptr, 0))
        {
            Assert(!m_rootSigObj, "Attempting to double-init.");
            m_rootSig.Finalize(name, m_rootSigObj, samplers, flags);
            m_psoLib.Init(name);
        }

        Core::PipelineStateLibrary m_psoLib;
        Core::RootSignature m_rootSig;
        ComPtr<ID3D12RootSignature> m_rootSigObj;

        ID3D12PipelineState* m_psoCache[nShaders] = { 0 };
    };
}