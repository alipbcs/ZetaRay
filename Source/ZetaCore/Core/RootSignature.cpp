#include "RootSignature.h"
#include "RendererCore.h"
#include "CommandList.h"
#include "GpuMemory.h"
#include "SharedShaderResources.h"
#include <xxHash/xxhash.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Util;

namespace
{
	template <typename T>
	requires std::same_as<T, GraphicsCmdList> || std::same_as<T, ComputeCmdList>
	void End_Internal(T& ctx, uint32_t rootCBVBitMap, uint32_t rootSRVBitMap, uint32_t rootUAVBitMap, 
		uint32_t globalsBitMap, uint32_t& modifiedBitMap, uint32_t& modifiedGlobalsBitMap, uint32_t optionalBitMap,
		int rootConstantsIdx, Span<uint32_t> rootConstants, Span<D3D12_GPU_VIRTUAL_ADDRESS> rootDescriptors,
		Span<uint64_t> globals)
	{
		// root constants
		if (rootConstantsIdx != -1 && (modifiedBitMap & (1 << rootConstantsIdx)))
		{
			ctx.SetRoot32BitConstants(rootConstantsIdx, (uint32_t)rootConstants.size(), rootConstants.data(), 0);
			modifiedBitMap ^= (1 << rootConstantsIdx);
		}

		uint32_t mask;
		DWORD nextParam;

		// root CBV
		mask = rootCBVBitMap;
		while (_BitScanForward(&nextParam, mask))
		{
			mask ^= (1 << nextParam);

			if ((1 << nextParam) & globalsBitMap)
				continue;

			if ((modifiedBitMap & (1 << nextParam)) == 0)
				continue;

			modifiedBitMap ^= (1 << nextParam);

			if (rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0))
				ctx.SetRootConstantBufferView(nextParam, rootDescriptors[nextParam]);
			else
				Assert(optionalBitMap & (1 << nextParam), "Root CBV in parameter %d has not been set", nextParam);
		}

		// root SRV
		mask = rootSRVBitMap;
		while (_BitScanForward(&nextParam, mask))
		{
			mask ^= (1 << nextParam);

			if ((1 << nextParam) & globalsBitMap)
				continue;

			if ((modifiedBitMap & (1 << nextParam)) == 0)
				continue;

			modifiedBitMap ^= (1 << nextParam);

			if (rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0))
				ctx.SetRootShaderResourceView(nextParam, rootDescriptors[nextParam]);
			else
				Assert(optionalBitMap & (1 << nextParam), "Root SRV in parameter %d has not been set", nextParam);
		}

		// root UAV
		mask = rootUAVBitMap;
		while (_BitScanForward(&nextParam, mask))
		{
			mask ^= (1 << nextParam);

			if ((1 << nextParam) & globalsBitMap)
				continue;

			if ((modifiedBitMap & (1 << nextParam)) == 0)
				continue;

			modifiedBitMap ^= (1 << nextParam);

			if (rootDescriptors[nextParam] != D3D12_GPU_VIRTUAL_ADDRESS(0))
				ctx.SetRootUnorderedAccessView(nextParam, rootDescriptors[nextParam]);
			else
				Assert(optionalBitMap & (1 << nextParam), "Root UAV in parameter %d has not been set", nextParam);
		}

		// globals
		SharedShaderResources& shared = App::GetRenderer().GetSharedShaderResources();

		while (_BitScanForward(&nextParam, modifiedGlobalsBitMap))
		{
			uint32_t rootBitMap = (1 << nextParam);
			modifiedGlobalsBitMap ^= rootBitMap;

			if (rootBitMap & rootCBVBitMap)
			{
				auto* defautlHeapBuff = shared.GetDefaultHeapBuffer(globals[nextParam]);
				if (defautlHeapBuff)
					ctx.SetRootConstantBufferView(nextParam, defautlHeapBuff->GpuVA());
				else
				{
					auto* uploadHeapBuff = shared.GetUploadHeapBuffer(globals[nextParam]);
					if (uploadHeapBuff)
						ctx.SetRootConstantBufferView(nextParam, uploadHeapBuff->GpuVA());
					else
						Assert(optionalBitMap & (1 << nextParam), "Global resource in parameter %d was not found.", nextParam);
				}
			}
			else if (rootBitMap & rootSRVBitMap)
			{
				auto* defautlHeapBuff = shared.GetDefaultHeapBuffer(globals[nextParam]);
				if (defautlHeapBuff)
					ctx.SetRootShaderResourceView(nextParam, defautlHeapBuff->GpuVA());
				else
				{
					auto* uploadHeapBuff = shared.GetUploadHeapBuffer(globals[nextParam]);
					if (uploadHeapBuff)
						ctx.SetRootShaderResourceView(nextParam, uploadHeapBuff->GpuVA());
					else
						Assert(optionalBitMap & (1 << nextParam), "Global resource in parameter %d was not found.", nextParam);
				}
			}
			else if (rootBitMap & rootUAVBitMap)
			{
				// UAV must be a default heap buffer
				auto* defautlHeapBuff = shared.GetDefaultHeapBuffer(globals[nextParam]);
				if (defautlHeapBuff)
					ctx.SetRootUnorderedAccessView(nextParam, defautlHeapBuff->GpuVA());
				else
					Assert(optionalBitMap & (1 << nextParam), "Global resource in parameter %d was not found.", nextParam);
			}
			else
				Assert(false, "Root global was not found.");
		}
	}
}

//--------------------------------------------------------------------------------------
// RootSignature
//--------------------------------------------------------------------------------------

RootSignature::RootSignature(int nCBV, int nSRV, int nUAV, int nGlobs, int nConsts)
	: m_numParams(nCBV + nSRV + nUAV + (nConsts > 0)),
	m_numCBVs(nCBV),
	m_numSRVs(nSRV),
	m_numUAVs(nUAV),
	m_numGlobals(nGlobs),
	m_numRootConstants(nConsts)
{
	Assert((nCBV + nSRV + nUAV) * 2 + nConsts <= 64, "A maximum of 64 DWORDS can be present at root signature.");
	Assert(nCBV + nSRV + nUAV + (nConsts > 0 ? 1 : 0) <= MAX_NUM_PARAMS, "Number of root parameters can't exceed MAX_NUM_PARAMS");
	Assert(nConsts <= MAX_NUM_ROOT_CONSTANTS, "Number of root constants can't exceed MAX_NUM_ROOT_CONSTANTS");
}

void RootSignature::InitAsConstants(uint32_t rootIdx, uint32_t numDwords, uint32_t registerNum,
	uint32_t registerSpace, D3D12_SHADER_VISIBILITY visibility)
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bounds.", rootIdx);
	Assert(m_numRootConstants == numDwords, "Given number of root constants doesn't match m_numRootConstants");

	m_params[rootIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	m_params[rootIdx].ShaderVisibility = visibility;
	m_params[rootIdx].Constants.Num32BitValues = numDwords;
	m_params[rootIdx].Constants.ShaderRegister = registerNum;
	m_params[rootIdx].Constants.RegisterSpace = registerSpace;
}

void RootSignature::InitAsCBV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace, 
	D3D12_ROOT_DESCRIPTOR_FLAGS flags, const char* id, bool isOptional, D3D12_SHADER_VISIBILITY visibility)
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bounds.", rootIdx);
	Assert((m_rootCBVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as CBV");
	Assert((m_rootSRVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as SRV");
	Assert((m_rootUAVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as UAV");
	Assert((m_globalsBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as Global");

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
	D3D12_ROOT_DESCRIPTOR_FLAGS flags, const char* id, bool isOptional, D3D12_SHADER_VISIBILITY visibility)
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bounds.", rootIdx);
	Assert((m_rootCBVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as CBV");
	Assert((m_rootSRVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as SRV");
	Assert((m_rootUAVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as UAV");
	Assert((m_globalsBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as Global");

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
	D3D12_ROOT_DESCRIPTOR_FLAGS flags, const char* id, bool isOptional, D3D12_SHADER_VISIBILITY visibility)
{
	Assert(rootIdx < m_numParams, "Root index %d is out of bounds.", rootIdx);
	Assert((m_rootCBVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as CBV");
	Assert((m_rootSRVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as SRV");
	Assert((m_rootUAVBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as UAV");
	Assert((m_globalsBitMap & (1 << rootIdx)) == 0, "root paramerter was already set as Global");

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
	Span<D3D12_STATIC_SAMPLER_DESC> samplers, D3D12_ROOT_SIGNATURE_FLAGS flags)
{
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc{};
	rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rootSigDesc.Desc_1_1.NumParameters = m_numParams;
	rootSigDesc.Desc_1_1.pParameters = m_params;
	rootSigDesc.Desc_1_1.NumStaticSamplers = UINT(samplers.size());
	rootSigDesc.Desc_1_1.pStaticSamplers = samplers.data();
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

void RootSignature::Begin()
{
	m_modifiedBitMap = (1 << m_numParams) - 1;

	// Given the assumption that globals don't get destroyed/recreated per-draw/dispatch call,
	// set each global to modified only at the beginning of each frame
	m_modifiedGlobalsBitMap = m_globalsBitMap;

	memset(m_rootDescriptors, 0, sizeof(D3D12_GPU_VIRTUAL_ADDRESS) * MAX_NUM_PARAMS);
}

void RootSignature::SetRootConstants(uint32_t offset, uint32_t num, void* data)
{
	Assert(offset + num <= m_numRootConstants, "out-of-bound write.");
	memcpy(&m_rootConstants[offset], data, sizeof(uint32_t) * num);

	m_modifiedBitMap |= (1 << m_rootConstantsIdx);
}

void RootSignature::SetRootCBV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	Assert((1 << rootIdx) & m_rootCBVBitMap, "root parameter %u was not set as root CBV", rootIdx);
	Assert(!((1 << rootIdx) & m_globalsBitMap), "root parameter %u was set as global.", rootIdx);

	m_rootDescriptors[rootIdx] = va;
	m_modifiedBitMap |= (1 << rootIdx);
}

void RootSignature::SetRootSRV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	Assert((1 << rootIdx) & m_rootSRVBitMap, "root parameter %u was not set as root SRV", rootIdx);
	Assert(!((1 << rootIdx) & m_globalsBitMap), "root parameter %u was set as global.", rootIdx);

	m_rootDescriptors[rootIdx] = va;
	m_modifiedBitMap |= (1 << rootIdx);
}

void RootSignature::SetRootUAV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	Assert((1 << rootIdx) & m_rootUAVBitMap, "root parameter %u was not set as root UAV", rootIdx);
	Assert(!((1 << rootIdx) & m_globalsBitMap), "root parameter %u was set as global.", rootIdx);

	m_rootDescriptors[rootIdx] = va;
	m_modifiedBitMap |= (1 << rootIdx);
}

void RootSignature::End(GraphicsCmdList& ctx)
{
	End_Internal(ctx, m_rootCBVBitMap, m_rootSRVBitMap, m_rootUAVBitMap, m_globalsBitMap, 
		m_modifiedBitMap, m_modifiedGlobalsBitMap, m_optionalBitMap, m_rootConstantsIdx, 
		Span(m_rootConstants, m_numRootConstants), m_rootDescriptors, m_globals);
}

void RootSignature::End(ComputeCmdList& ctx)
{
	End_Internal(ctx, m_rootCBVBitMap, m_rootSRVBitMap, m_rootUAVBitMap, m_globalsBitMap,
		m_modifiedBitMap, m_modifiedGlobalsBitMap, m_optionalBitMap, m_rootConstantsIdx,
		Span(m_rootConstants, m_numRootConstants), m_rootDescriptors, m_globals);
}
