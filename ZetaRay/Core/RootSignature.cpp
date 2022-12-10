#include "RootSignature.h"
#include "../Utility/Error.h"
#include "Renderer.h"
#include "CommandList.h"
#include "GpuMemory.h"
#include "SharedShaderResources.h"
#include <xxHash-0.8.1/xxhash.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;

RootSignature::RootSignature(int nCBV, int nSRV, int nUAV, int nGlobs, int nConsts) noexcept
	: m_numParams(nCBV + nSRV + nUAV + (nConsts > 0 ? 1 : 0)),
	m_numCBVs(nCBV),
	m_numSRVs(nSRV),
	m_numUAVs(nUAV),
	m_numGlobals(nGlobs),
	m_numRootConstants(nConsts)
{
	Assert((nCBV + nSRV + nUAV) * 2 + nConsts <= 64, "A maximum of 64 DWORDS can be present at root signature.");
	Assert(nCBV + nSRV + nUAV + (nConsts > 0 ? 1 : 0) <= MAX_NUM_PARAMS, "Number of root parameters can't exceed MAX_NUM_PARAMS");
	Assert(nCBV + nSRV + nUAV <= MAX_NUM_ROOT_DESCRIPTORS,
		"Number of root descriptors can't exceed MAX_NUM_ROOT_DESCRIPTORS");
	Assert(nConsts <= MAX_NUM_ROOT_CONSTANTS, "Number of root constants can't exceed MAX_NUM_ROOT_CONSTANTS");
}

void RootSignature::InitAsConstants(uint32_t rootIdx, uint32_t numDwords, uint32_t registerNum,
	uint32_t registerSpace, D3D12_SHADER_VISIBILITY visibility) noexcept
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bound.", rootIdx);
	Assert(m_numRootConstants == numDwords, "Given number of root constants doesn't match m_numRootConstants");

	//m_params[rootIdx].InitAsConstants(numDwords, registerNum, registerSpace, visibility);
	m_params[rootIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	m_params[rootIdx].ShaderVisibility = visibility;
	m_params[rootIdx].Constants.Num32BitValues = numDwords;
	m_params[rootIdx].Constants.ShaderRegister = registerNum;
	m_params[rootIdx].Constants.RegisterSpace = registerSpace;
}

void RootSignature::InitAsCBV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace, 
	D3D12_ROOT_DESCRIPTOR_FLAGS flags, D3D12_SHADER_VISIBILITY visibility, const char* id, bool isOptional) noexcept
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bound.", rootIdx);
	Assert((m_rootCBVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as CBV");
	Assert((m_rootSRVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as SRV");
	Assert((m_rootUAVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as UAV");
	Assert((m_globalsBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as Global");

	//m_params[rootIdx].InitAsConstantBufferView(registerNum, registerSpace, flag, visibility);
	m_params[rootIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	m_params[rootIdx].ShaderVisibility = visibility;
	m_params[rootIdx].Descriptor.ShaderRegister = registerNum;
	m_params[rootIdx].Descriptor.RegisterSpace = registerSpace;
	m_params[rootIdx].Descriptor.Flags = flags;

	m_rootCBVBitMap |= (1 << rootIdx);

	if (id)
	{
		m_globals[rootIdx] = XXH3_64bits(id, strlen(id));
		m_globalsBitMap |= (1 << rootIdx);
	}

	if (isOptional)
		m_optionalBitMap |= (1 << rootIdx);
}

void RootSignature::InitAsBufferSRV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace, 
	D3D12_ROOT_DESCRIPTOR_FLAGS flags, D3D12_SHADER_VISIBILITY visibility, const char* id, bool isOptional) noexcept
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bound.", rootIdx);
	Assert((m_rootCBVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as CBV");
	Assert((m_rootSRVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as SRV");
	Assert((m_rootUAVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as UAV");
	Assert((m_globalsBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as Global");

	//m_params[rootIdx].InitAsShaderResourceView(registerNum, registerSpace, flag, visibility);
	m_params[rootIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	m_params[rootIdx].ShaderVisibility = visibility;
	m_params[rootIdx].Descriptor.ShaderRegister = registerNum;
	m_params[rootIdx].Descriptor.RegisterSpace = registerSpace;
	m_params[rootIdx].Descriptor.Flags = flags;

	m_rootSRVBitMap |= (1 << rootIdx);
	
	if (id)
	{
		m_globals[rootIdx] = XXH3_64bits(id, strlen(id));
		m_globalsBitMap |= (1 << rootIdx);
	}

	if (isOptional)
		m_optionalBitMap |= (1 << rootIdx);
}

void RootSignature::InitAsBufferUAV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace, 
	D3D12_ROOT_DESCRIPTOR_FLAGS flags, D3D12_SHADER_VISIBILITY visibility, const char* id, bool isOptional) noexcept
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bound.", rootIdx);
	Assert((m_rootCBVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as CBV");
	Assert((m_rootSRVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as SRV");
	Assert((m_rootUAVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as UAV");
	Assert((m_globalsBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as Global");

	//m_params[rootIdx].InitAsUnorderedAccessView(registerNum, registerSpace, flag, visibility);
	m_params[rootIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
	m_params[rootIdx].ShaderVisibility = visibility;
	m_params[rootIdx].Descriptor.ShaderRegister = registerNum;
	m_params[rootIdx].Descriptor.RegisterSpace = registerSpace;
	m_params[rootIdx].Descriptor.Flags = flags;

	m_rootUAVBitMap |= (1 << rootIdx);

	if (id)
	{
		m_globals[rootIdx] = XXH3_64bits(id, strlen(id));
		m_globalsBitMap |= (1 << rootIdx);
	}

	if (isOptional)
		m_optionalBitMap |= (1 << rootIdx);
}

void RootSignature::Finalize(const char* name, ComPtr<ID3D12RootSignature>& rootSig, 
	UINT numStaticSamplers, const D3D12_STATIC_SAMPLER_DESC* samplers, D3D12_ROOT_SIGNATURE_FLAGS flags)
{
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc{};
	//rootSigDesc.Init_1_1(NumParams(), m_params.data(), numStaticSamplers, samplers, flags);
	rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rootSigDesc.Desc_1_1.NumParameters = m_numParams;
	rootSigDesc.Desc_1_1.pParameters = m_params;
	rootSigDesc.Desc_1_1.NumStaticSamplers = numStaticSamplers;
	rootSigDesc.Desc_1_1.pStaticSamplers = samplers;
	rootSigDesc.Desc_1_1.Flags = flags;

	ComPtr<ID3DBlob> pOutBlob, pErrorBlob;
	HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, pOutBlob.GetAddressOf(), pErrorBlob.GetAddressOf());

	if(FAILED(hr))
		Check(false, "D3D12SerializeVersionedRootSignature() failed: %s", (char*)pErrorBlob->GetBufferPointer());

	auto* device = App::GetRenderer().GetDevice();
	CheckHR(device->CreateRootSignature(
		0,
		pOutBlob->GetBufferPointer(),
		pOutBlob->GetBufferSize(),
		IID_PPV_ARGS(rootSig.GetAddressOf())));

	Assert(name, "name was NULL");
	rootSig->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);

	// calculate root parameter index for root constants (if any)
	uint32_t u = (1 << m_numParams) - 1;		// set the first NumParams() bits to 1
	u &= (m_rootCBVBitMap | m_rootSRVBitMap | m_rootUAVBitMap | m_globalsBitMap);

	DWORD idx;
	m_rootConstantsIdx = _BitScanForward(&idx, ~u) && (idx < m_numParams) ? (int)idx : -1;
}

void RootSignature::Begin() noexcept
{
	m_modifiedBitMap = (1 << m_numParams) - 1;

	// Given the assumption that globals don't get destroyed/recreated per-draw/dispatch call,
	// set each global to modified only at the beginning of each frame
	m_modifiedGlobalsBitMap = m_globalsBitMap;

	memset(m_rootDescriptors, 0, sizeof(D3D12_GPU_VIRTUAL_ADDRESS) * MAX_NUM_ROOT_DESCRIPTORS);
}

void RootSignature::SetRootConstants(uint32_t offset, uint32_t num, void* data) noexcept
{
	Assert(offset + num <= m_numRootConstants, "out-of-bound write.");
	memcpy(&m_rootConstants[offset], data, sizeof(uint32_t) * num);

	m_modifiedBitMap |= (1 << m_rootConstantsIdx);
}

void RootSignature::SetRootCBV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept
{
	Assert((1 << rootIdx) & m_rootCBVBitMap, "root parameter %u was not set as root CBV", rootIdx);
	Assert(!((1 << rootIdx) & m_globalsBitMap), "root parameter %u was set as global.", rootIdx);

	m_rootDescriptors[rootIdx] = va;
	m_modifiedBitMap |= (1 << rootIdx);
}

void RootSignature::SetRootSRV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept
{
	Assert((1 << rootIdx) & m_rootSRVBitMap, "root parameter %u was not set as root SRV", rootIdx);
	Assert(!((1 << rootIdx) & m_globalsBitMap), "root parameter %u was set as global.", rootIdx);

	m_rootDescriptors[rootIdx] = va;
	m_modifiedBitMap |= (1 << rootIdx);
}

void RootSignature::SetRootUAV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va) noexcept
{
	Assert((1 << rootIdx) & m_rootUAVBitMap, "root parameter %u was not set as root UAV", rootIdx);
	Assert(!((1 << rootIdx) & m_globalsBitMap), "root parameter %u was set as global.", rootIdx);

	m_rootDescriptors[rootIdx] = va;
	m_modifiedBitMap |= (1 << rootIdx);
}

void RootSignature::End(GraphicsCmdList& ctx) noexcept
{
	// root constants
	if (m_rootConstantsIdx != -1 && (m_modifiedBitMap & (1 << m_rootConstantsIdx)))
	{
		ctx.SetRoot32BitConstants(m_rootConstantsIdx, m_numRootConstants, m_rootConstants, 0);
		m_modifiedBitMap ^= (1 << m_rootConstantsIdx);
	}

	uint32_t mask;
	DWORD nextParam;

	// root CBV
	mask = m_rootCBVBitMap;
	while (_BitScanForward(&nextParam, mask))
	{
		mask ^= (1 << nextParam);

		if ((1 << nextParam) & m_globalsBitMap)
			continue;

		if ((m_modifiedBitMap & (1 << nextParam)) == 0)
			continue;

		m_modifiedBitMap ^= (1 << nextParam);
		Assert(m_rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0), "Root CBV in parameter %d has not been set", nextParam);
		ctx.SetRootConstantBufferView(nextParam, m_rootDescriptors[nextParam]);
	}

	// root SRV
	mask = m_rootSRVBitMap;
	while (_BitScanForward(&nextParam, mask))
	{
		mask ^= (1 << nextParam);

		if ((1 << nextParam) & m_globalsBitMap)
			continue;

		if ((m_modifiedBitMap & (1 << nextParam)) == 0)
			continue;

		m_modifiedBitMap ^= (1 << nextParam);
		Assert(m_rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0), "Root SRV in parameter %d has not been set", nextParam);
		ctx.SetRootShaderResourceView(nextParam, m_rootDescriptors[nextParam]);
	}

	// root UAV
	mask = m_rootUAVBitMap;
	while (_BitScanForward(&nextParam, mask))
	{
		mask ^= (1 << nextParam);

		if ((1 << nextParam) & m_globalsBitMap)
			continue;

		if ((m_modifiedBitMap & (1 << nextParam)) == 0)
			continue;

		m_modifiedBitMap ^= (1 << nextParam);
		Assert(m_rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0), "Root UAV in parameter %d has not been set", nextParam);
		ctx.SetRootUnorderedAccessView(nextParam, m_rootDescriptors[nextParam]);
	}

	// globals
	SharedShaderResources& shared = App::GetRenderer().GetSharedShaderResources();

	while (_BitScanForward(&nextParam, m_modifiedGlobalsBitMap))
	{
		uint32_t rootBitMap = (1 << nextParam);
		m_modifiedGlobalsBitMap ^= rootBitMap;

		if (rootBitMap & m_rootCBVBitMap)
		{
			D3D12_GPU_VIRTUAL_ADDRESS va;
			auto* uploadHeapBuff = shared.GetUploadHeapBuff(m_globals[nextParam]);
			if (!uploadHeapBuff)
			{
				auto* defautlHeapBuff = shared.GetDefaultHeapBuff(m_globals[nextParam]);
				Assert(defautlHeapBuff && defautlHeapBuff->IsInitialized(), "Global resource with id %llu was not found.", m_globals[nextParam]);

				va = defautlHeapBuff->GetGpuVA();
			}
			else
				va = uploadHeapBuff->GetGpuVA();

			ctx.SetRootConstantBufferView(nextParam, va);
		}
		else if (rootBitMap & m_rootSRVBitMap)
		{
			D3D12_GPU_VIRTUAL_ADDRESS va;
			auto* uploadHeapBuff = shared.GetUploadHeapBuff(m_globals[nextParam]);
			if (!uploadHeapBuff)
			{
				auto* defautlHeapBuff = shared.GetDefaultHeapBuff(m_globals[nextParam]);
				Assert(defautlHeapBuff && defautlHeapBuff->IsInitialized(), "Global resource with id %llu was not found.", m_globals[nextParam]);

				va = defautlHeapBuff->GetGpuVA();
			}
			else
				va = uploadHeapBuff->GetGpuVA();

			ctx.SetRootShaderResourceView(nextParam, va);
		}
		else if (rootBitMap & m_rootUAVBitMap)
		{
			// UAV must be a default-heap buffer
			auto* defautlHeapBuff = shared.GetDefaultHeapBuff(m_globals[nextParam]);
			Assert(defautlHeapBuff && defautlHeapBuff->IsInitialized(), "Root param %d: Global resource with id %llu was not found.", nextParam, m_globals[nextParam]);
			D3D12_GPU_VIRTUAL_ADDRESS va = defautlHeapBuff->GetGpuVA();
			ctx.SetRootUnorderedAccessView(nextParam, va);
		}
		else
			Assert(false, "Root global was not found.");
	}
}

void RootSignature::End(ComputeCmdList& ctx) noexcept
{
	// root constants
	if (m_rootConstantsIdx != -1 && (m_modifiedBitMap & (1 << m_rootConstantsIdx)))
	{
		ctx.SetRoot32BitConstants(m_rootConstantsIdx, m_numRootConstants, m_rootConstants, 0);
		m_modifiedBitMap ^= (1 << m_rootConstantsIdx);
	}

	uint32_t mask;
	DWORD nextParam;

	// root CBV
	mask = m_rootCBVBitMap;
	while (_BitScanForward(&nextParam, mask))
	{
		mask ^= (1 << nextParam);

		if ((1 << nextParam) & m_globalsBitMap)
			continue;

		if ((m_modifiedBitMap & (1 << nextParam)) == 0)
			continue;

		m_modifiedBitMap ^= (1 << nextParam);
		Assert(m_optionalBitMap & (1 << nextParam) || m_rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0), "Root CBV in parameter %d has not been set", nextParam);
		ctx.SetRootConstantBufferView(nextParam, m_rootDescriptors[nextParam]);
	}

	// root SRV
	mask = m_rootSRVBitMap;
	while (_BitScanForward(&nextParam, mask))
	{
		mask ^= (1 << nextParam);

		if ((1 << nextParam) & m_globalsBitMap)
			continue;

		if ((m_modifiedBitMap & (1 << nextParam)) == 0)
			continue;

		m_modifiedBitMap ^= (1 << nextParam);
		Assert(m_optionalBitMap & (1 << nextParam) || m_rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0), "Root SRV in parameter %d has not been set", nextParam);
		ctx.SetRootShaderResourceView(nextParam, m_rootDescriptors[nextParam]);
	}

	// root UAV
	mask = m_rootUAVBitMap;
	while (_BitScanForward(&nextParam, mask))
	{
		mask ^= (1 << nextParam);

		if ((1 << nextParam) & m_globalsBitMap)
			continue;

		if ((m_modifiedBitMap & (1 << nextParam)) == 0)
			continue;

		m_modifiedBitMap ^= (1 << nextParam);
		Assert(m_optionalBitMap & (1 << nextParam) || m_rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0), "Root UAV in parameter %d has not been set", nextParam);
		ctx.SetRootUnorderedAccessView(nextParam, m_rootDescriptors[nextParam]);
	}

	// globals
	SharedShaderResources& shared = App::GetRenderer().GetSharedShaderResources();

	while (_BitScanForward(&nextParam, m_modifiedGlobalsBitMap))
	{
		uint32_t rootBitMap = (1 << nextParam);
		m_modifiedGlobalsBitMap ^= rootBitMap;

		if (rootBitMap & m_rootCBVBitMap)
		{
			D3D12_GPU_VIRTUAL_ADDRESS va;
			auto* uploadHeapBuff = shared.GetUploadHeapBuff(m_globals[nextParam]);
			if (!uploadHeapBuff)
			{
				auto* defautlHeapBuff = shared.GetDefaultHeapBuff(m_globals[nextParam]);
				Assert(defautlHeapBuff && defautlHeapBuff->IsInitialized(), "Global resource with id %llu was not found.", m_globals[nextParam]);

				va = defautlHeapBuff->GetGpuVA();
			}
			else
				va = uploadHeapBuff->GetGpuVA();

			ctx.SetRootConstantBufferView(nextParam, va);
		}
		else if (rootBitMap & m_rootSRVBitMap)
		{
			D3D12_GPU_VIRTUAL_ADDRESS va;
			auto* uploadHeapBuff = shared.GetUploadHeapBuff(m_globals[nextParam]);
			if (!uploadHeapBuff)
			{
				auto* defautlHeapBuff = shared.GetDefaultHeapBuff(m_globals[nextParam]);
				Assert(defautlHeapBuff && defautlHeapBuff->IsInitialized(), "Global resource with id %llu was not found.", m_globals[nextParam]);

				va = defautlHeapBuff->GetGpuVA();
			}
			else
				va = uploadHeapBuff->GetGpuVA();

			ctx.SetRootShaderResourceView(nextParam, va);
		}
		else if (rootBitMap & m_rootUAVBitMap)
		{
			// UAV must be a default-heap buffer
			auto* defautlHeapBuff = shared.GetDefaultHeapBuff(m_globals[nextParam]);
			Assert(defautlHeapBuff && defautlHeapBuff->IsInitialized(), "Root param %d: Global resource with id %llu was not found.", nextParam, m_globals[nextParam]);
			D3D12_GPU_VIRTUAL_ADDRESS va = defautlHeapBuff->GetGpuVA();
			ctx.SetRootUnorderedAccessView(nextParam, va);
		}
		else
			Assert(false, "Root global was not found.");
	}
}

/*
void RootSignature::Reset() noexcept
{
	m_rootCBVBitMap = 0;
	m_rootSRVBitMap = 0;
	m_rootUAVBitMap = 0;
	m_globalsBitMap = 0;
	m_optionalBitMap = 0;
	m_rootConstantsIdx = -1;
	m_modifiedBitMap = 0;
	m_modifiedGlobalsBitMap = 0;
}
*/