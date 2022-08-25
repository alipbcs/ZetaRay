#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "Reduction_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct LuminanceReduction final
	{
		enum class SHADER_IN_DESC
		{
			COMPOSITED,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			AVG_LUM,
			COUNT
		};

		LuminanceReduction() noexcept;
		~LuminanceReduction() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_psos[0] != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		void SetDescriptor(SHADER_IN_DESC i, uint32_t heapIdx) noexcept
		{
			Assert((int)i < (int)SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_inputDesc[(int)i] = heapIdx;
		}
		Core::DefaultHeapBuffer& GetOutput(SHADER_OUT_RES i) noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_reducedLum;
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void CreateResources() noexcept;

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 1;
		static constexpr int NUM_UAV = 1;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbReduction) / sizeof(DWORD);

		inline static RpObjects s_rpObjs;

		inline static const char* COMPILED_CS[] = { "ReductionFirst_cs.cso", "ReductionSecond_cs.cso" };

		Core::DefaultHeapBuffer m_reducedLumIntermediate;
		Core::DefaultHeapBuffer m_reducedLum;

		uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT] = { 0 };

		// both passes use the same Root Signature
		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_psos[2] = { nullptr };
	};
}