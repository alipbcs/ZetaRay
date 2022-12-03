#include "RenderPass.h"
#include "../Core/RootSignature.h"
#include "../Core/Renderer.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;

void RpObjects::Init(const char* name,
	RootSignature& rootSigInstance,
	int numStaticSamplers,
	const D3D12_STATIC_SAMPLER_DESC* samplers,
	D3D12_ROOT_SIGNATURE_FLAGS flags) noexcept
{
	const int prev = m_refCount++;

	// initialize if object haven't been initialized yet
	if (prev == 0)
	{
		rootSigInstance.Finalize(name, m_rootSig, numStaticSamplers, samplers, flags);
		m_psoLib.Init(name);

		CheckHR(App::GetRenderer().GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
	}
}

void RpObjects::Clear() noexcept
{
	const int prev = m_refCount--;

	if (prev == 1)
	{
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
	}
}