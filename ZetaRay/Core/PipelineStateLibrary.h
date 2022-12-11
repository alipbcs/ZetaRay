#pragma once

#include "../Core/Device.h"
#include "../Utility/SmallVector.h"
#include "../App/Filesystem.h"
#include <atomic>

namespace ZetaRay::Core
{
	class PipelineStateLibrary
	{
	public:
		PipelineStateLibrary() noexcept = default;
		~PipelineStateLibrary() noexcept;

		PipelineStateLibrary(const PipelineStateLibrary&) = delete;
		PipelineStateLibrary& operator=(const PipelineStateLibrary&) = delete;

		void Init(const char* name) noexcept;
		
		// Warning: Shouldn't be called while the GPU is still referencing the contained PSOs
		void ClearAndFlushToDisk() noexcept;

		// Warning: Calling Reload for RenderPasses that have more than one instance will lead
		// to use-after-free bug
		void Reload(uint64_t nameID, const char* pathToHlsl, bool isComputePSO) noexcept;

		ID3D12PipelineState* GetGraphicsPSO(uint64_t nameID,
			D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc,
			ID3D12RootSignature* rootSig,
			const char* pathToCompiledVS,
			const char* pathToCompiledPS) noexcept;

		ID3D12PipelineState* GetComputePSO(uint64_t nameID,
			ID3D12RootSignature* rootSig,
			const char* pathToCompiledCS) noexcept;		
		
		ID3D12PipelineState* GetComputePSO(uint64_t nameID,
			ID3D12RootSignature* rootSig,
			Util::Span<const uint8_t> compiledBlob) noexcept;

	private:
		struct Entry
		{
			uint64_t Key;
			ID3D12PipelineState* PSO;
		};

		// Following functions need to be synhronized across threads. This is assumed
		// to be done by the caller
		ID3D12PipelineState* Find(uint64_t key) noexcept;
		void InsertPSOAndKeepSorted(Entry e) noexcept;
		bool UpdatePSO(Entry e) noexcept;
		bool RemovePSO(uint64_t nameID) noexcept;

		void DeletePsoLibFile() noexcept;
		void ResetPsoLib(bool forceReset = false) noexcept;

		//char m_psoLibPath[MAX_PATH] = { '\0' };			// <Assets>/PSOCache/<name>.cache	
		App::Filesystem::Path m_psoLibPath1;
		ComPtr<ID3D12PipelineLibrary> m_psoLibrary;

		Util::SmallVector<Entry, App::ThreadAllocator, 2> m_compiledPSOs;
		Util::SmallVector<uint8_t, App::ThreadAllocator> m_cachedBlob;
		
		bool m_foundOnDisk = false;
		bool m_psoWasReset = false;
	};
}