#pragma once

#include "Device.h"

namespace ZetaRay::Util
{
	template<typename T>
	struct Span;
}

namespace ZetaRay::Core
{
	class GraphicsCmdList;
	class ComputeCmdList;

	// All the needed scenarios
	// 
	// 1. Upload heap buffer (read-only, GENERIC_READ)
	//		a. constant buffer
	//			I. local -> root CBV -> Set*RootConstantBufferView(GpuVA)
	//			II. global -> root CBV (sharedshaderres has the buff and provides GpuVA)
	// 
	//		b. structured buffer (consider copying to default heap?)
	//			I. local -> root SRV -> Set*RootShaderResourceView(GpuVA)
	//						(UAV is not needed)
	//			II. global -> root SRV (sharedshaderres has the buff and provides GpuVA)
	//						(UAV is not needed)
	// 
	// 2. Default heap buffer
	//		a. structured buffer
	//			I. local -> root SRV -> Set*RootShaderResourceView(GpuVA)
	//					|
	//					 -> root UAV -> Set*RootUnorderedAccessView(GpuVA)
	// 
	//			II. global -> root SRV (sharedshaderres has the buff and provides GpuVA)
	//				      |
	//					   -> root UAV -> Set*RootUnorderedAccessView(GpuVA)
	//
	// 3. Texture
	//		a. local -> create descriptor and store heap idx in a root CBV or a root constant
	//		b. global -> descriptor table is already created. desc table headp idx goes in a root CBV or a root constant
	//
	// In conclusion, root signatures only need root CBV, root SRV, root UAV and root constants.
	//
	// ASSUMPTION: globals only change once per-frame, which means they should not change
	// in-between draw/dispatch calls. Begin() marks them as modified, but once they're set,
	// they can't be modified again.
	//

	struct RootSignature
	{
		RootSignature(int nCBV, int nSRV, int nUAV, int nGlobs, int nConsts);
		~RootSignature() = default;

		RootSignature(const RootSignature&) = delete;
		RootSignature& operator=(const RootSignature&) = delete;

		void InitAsConstants(uint32_t rootIdx, uint32_t numDwords, uint32_t registerNum,
			uint32_t registerSpace, D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL);

		void InitAsCBV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags, D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL,
			const char* id = nullptr, bool isOptional = false);

		void InitAsBufferSRV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags, D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL,
			const char* id = nullptr, bool isOptional = false);

		void InitAsBufferUAV(uint32_t rootIdx, uint32_t registerNum, uint32_t registerSpace,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags, D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL,
			const char* id = nullptr, bool isOptional = false);

		void Finalize(const char* name,
			ComPtr<ID3D12RootSignature>& rootSig,
			Util::Span<D3D12_STATIC_SAMPLER_DESC> samplers,
			D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE);

		void Begin();

		void SetRootConstants(uint32_t offset, uint32_t num, void* data);
		void SetRootCBV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va);
		void SetRootSRV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va);
		void SetRootUAV(uint32_t rootIdx, D3D12_GPU_VIRTUAL_ADDRESS va);

		void End(GraphicsCmdList& ctx);
		void End(ComputeCmdList& ctx);

	private:
		static constexpr int MAX_NUM_PARAMS = 11;
		//static constexpr int MAX_NUM_ROOT_DESCRIPTORS = 9;
		static constexpr int MAX_NUM_ROOT_CONSTANTS = 24;

		const uint32_t m_numParams;
		const uint32_t m_numCBVs;
		const uint32_t m_numSRVs;
		const uint32_t m_numUAVs;
		const uint32_t m_numGlobals;
		const uint32_t m_numRootConstants;

		D3D12_ROOT_PARAMETER1 m_params[MAX_NUM_PARAMS];

		// buffer IDs
		uint64_t m_globals[MAX_NUM_PARAMS];

		// bitmaps indicating the type of each root parameter
		uint32_t m_rootCBVBitMap = 0;
		uint32_t m_rootSRVBitMap = 0;
		uint32_t m_rootUAVBitMap = 0;

		// Bitmap indicating which root params are global resources
		uint32_t m_globalsBitMap = 0;
		// Bitmap indicating which root params are optional
		uint32_t m_optionalBitMap = 0;

		// Index of the root constants param (there can be at most one)
		int m_rootConstantsIdx = -1;

		// root descriptors
		D3D12_GPU_VIRTUAL_ADDRESS m_rootDescriptors[MAX_NUM_PARAMS] = { 0 };

		// root constants data
		uint32_t m_rootConstants[MAX_NUM_ROOT_CONSTANTS];

		// Ref: https://www.intel.com/content/www/us/en/developer/articles/technical/introduction-to-resource-binding-in-microsoft-directx-12.html
		// "All the root parameters like descriptor tables, root descriptors, and root constants 
		// are baked in to a command list and the driver will be versioning them on behalf of the 
		// application. In other words, whenever any of the root parameters change between draw or 
		// dispatch calls, the hardware will update the version number of the root signature. Every 
		// draw / dispatch call gets a unique full set of root parameter states when any argument 
		// changes."
		uint32_t m_modifiedBitMap = 0;
		uint32_t m_modifiedGlobalsBitMap = 0;
	};
}