#pragma once

#include <Core/PipelineStateLibrary.h>
#include <Core/RootSignature.h>

namespace ZetaRay::Core
{
	struct RootSignature;
}

namespace ZetaRay::RenderPass
{
	struct RenderPassBase
	{
	protected:
		RenderPassBase(int nRootCBV, int nRootSRV, int nRootUAV, int nRootGlobs, int nRootConsts);
		~RenderPassBase();

		RenderPassBase(RenderPassBase&&) = delete;
		RenderPassBase& operator=(RenderPassBase&&) = delete;

		void InitRenderPass(const char* name,
			D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
			Util::Span<D3D12_STATIC_SAMPLER_DESC> samplers = Util::Span<D3D12_STATIC_SAMPLER_DESC>(nullptr, 0));

		void ResetRenderPass();

		Core::PipelineStateLibrary m_psoLib;

		Core::RootSignature m_rootSig;
		ComPtr<ID3D12RootSignature> m_rootSigObj;

		// destruction should be guarded by a fence to make sure GPU is done with 
		// the objects used by this renderpass
		ComPtr<ID3D12Fence> m_fence;
	};
}