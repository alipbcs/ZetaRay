#pragma once

#include "../Core/Device.h"
#include "../App/Filesystem.h"
#include <atomic>

namespace ZetaRay::Core
{
	class PipelineStateLibrary
	{
	public:
		PipelineStateLibrary() = default;
		~PipelineStateLibrary();

		PipelineStateLibrary(const PipelineStateLibrary&) = delete;
		PipelineStateLibrary& operator=(const PipelineStateLibrary&) = delete;

		void Init(const char* name);
		
		// Warning: shouldn't be called while the GPU is still referencing the contained PSOs
		void ClearAndFlushToDisk();
		void Reload(uint64_t nameID, const char* pathToHlsl, bool isComputePSO);

		ID3D12PipelineState* GetGraphicsPSO(uint64_t nameID,
			D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc,
			ID3D12RootSignature* rootSig,
			const char* pathToCompiledVS,
			const char* pathToCompiledPS);

		ID3D12PipelineState* GetComputePSO(uint64_t nameID,
			ID3D12RootSignature* rootSig,
			const char* pathToCompiledCS);		
		
		ID3D12PipelineState* GetComputePSO(uint64_t nameID,
			ID3D12RootSignature* rootSig,
			Util::Span<const uint8_t> compiledBlob);

	private:
		struct Entry
		{
			uint64_t Key;
			ID3D12PipelineState* PSO;
		};

		// following functions need to be synhronized across threads. This is assumed
		// to be done by the caller
		ID3D12PipelineState* Find(uint64_t key);
		void InsertPSOAndKeepSorted(Entry e);
		bool UpdatePSO(Entry e);
		bool RemovePSO(uint64_t nameID);

		void DeletePsoLibFile();
		void ResetPsoLib(bool forceReset = false);

		//char m_psoLibPath[MAX_PATH] = { '\0' };			// <Assets>/PSOCache/<name>.cache	
		App::Filesystem::Path m_psoLibPath1;
		ComPtr<ID3D12PipelineLibrary> m_psoLibrary;

		Util::SmallVector<Entry, Support::SystemAllocator, 2> m_compiledPSOs;
		Util::SmallVector<uint8_t> m_cachedBlob;
		
		bool m_foundOnDisk = false;
		bool m_psoWasReset = false;
	};
}