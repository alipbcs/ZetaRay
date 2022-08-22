#include "RenderPass.h"
#include "../Core/RootSignature.h"
#include "../Core/Renderer.h"
#include ".../../../Win32/App.h"

using namespace ZetaRay;
using namespace ZetaRay::RenderPass;

void RpObjects::Init(const char* name,
	RootSignature& rootSigInstance,
	int numStaticSamplers,
	const D3D12_STATIC_SAMPLER_DESC* samplers,
	D3D12_ROOT_SIGNATURE_FLAGS flags) noexcept
{
	const int prev = m_refCount.fetch_add(1, std::memory_order_acq_rel);

	// initialize if object haven't been initialized yet
	if (prev == 0)
	{
		rootSigInstance.Finalize(name, m_rootSig, numStaticSamplers, samplers, flags);
		m_psoLib.Init(name);

		CheckHR(App::GetRenderer().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));

		// set init flag to true
		m_initFlag.store(true, std::memory_order_release);
		m_initFlag.notify_all();
	}

	// wait if another instance is performing initialization for the first time
	m_initFlag.wait(false, std::memory_order_acquire);
}

void RpObjects::Clear() noexcept
{
	const int prev = m_refCount.fetch_sub(1, std::memory_order_relaxed);

	if (prev == 1)
	{
		App::GetRenderer().SignalDirectQueue(m_fence.Get(), 1);

		if (m_fence->GetCompletedValue() < 1)
		{
			HANDLE handle = CreateEventA(nullptr, false, false, "");
			AssertWin32(handle);
			CheckHR(m_fence->SetEventOnCompletion(1, handle));

			WaitForSingleObject(handle, INFINITE);
			CloseHandle(handle);

		}

		m_psoLib.ClearnAndFlushToDisk();
		m_initFlag.store(false, std::memory_order_release);
	}
}