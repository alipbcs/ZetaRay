#pragma once

#include <Core/PipelineStateLibrary.h>

namespace ZetaRay::Core
{
	struct RootSignature;
}

namespace ZetaRay::RenderPass
{
	struct RpObjects
	{
		void Init(const char* name,
			Core::RootSignature& rootSigInstance,
			size_t numStaticSamplers = 0,
			const D3D12_STATIC_SAMPLER_DESC* samplers = nullptr,
			D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE);

		void Clear();

		Core::PipelineStateLibrary m_psoLib;
		ComPtr<ID3D12RootSignature> m_rootSig;

		// destruction should be guarded by a fence to make sure GPU is done with 
		// the objects used by this renderpass
		ComPtr<ID3D12Fence> m_fence;

		int m_refCount = 0;
	};
}