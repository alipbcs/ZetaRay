#include "RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/RendererCore.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// RenderPassBase
//--------------------------------------------------------------------------------------

RenderPassBase::RenderPassBase(int nRootCBV, int nRootSRV, int nRootUAV, int nRootGlobs, int nRootConsts)
    : m_rootSig(nRootCBV, nRootSRV, nRootUAV, nRootGlobs, nRootConsts)
{}

RenderPassBase::~RenderPassBase()
{
    ResetRenderPass();
}

void RenderPassBase::InitRenderPass(const char* name, D3D12_ROOT_SIGNATURE_FLAGS flags, 
    Span<D3D12_STATIC_SAMPLER_DESC> samplers)
{
    Assert(!m_rootSigObj, "Attempting to double-init.");
    m_rootSig.Finalize(name, m_rootSigObj, samplers, flags);
    m_psoLib.Init(name);

    CheckHR(App::GetRenderer().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
}

void RenderPassBase::ResetRenderPass()
{
    if (!m_rootSigObj)
        return;

    App::GetRenderer().SignalDirectQueue(m_fence.Get(), 1);

    if (m_fence->GetCompletedValue() < 1)
    {
        HANDLE handle = CreateEventA(nullptr, false, false, "");
        CheckWin32(handle);
        CheckHR(m_fence->SetEventOnCompletion(1, handle));

        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }

    m_psoLib.ClearAndFlushToDisk();
    uint32_t reftCount = m_rootSigObj.Reset();
    Assert(reftCount == 0, "unexpected ref count.");
}