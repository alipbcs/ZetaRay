#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "TAA_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::Support
{
	struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
	struct TAA final
	{
		enum class SHADER_IN_DESC
		{
			SIGNAL,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			OUTPUT_A,
			OUTPUT_B,
			COUNT
		};

		TAA() noexcept;
		~TAA() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; }
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		void SetDescriptor(SHADER_IN_DESC i, uint32_t heapIdx) noexcept
		{
			Assert((int)i < (int)SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_inputDesc[(int)i] = heapIdx;
		}
		Core::Texture& GetOutput(SHADER_OUT_RES i) noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_antiAliased[(int)i];
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void CreateResources() noexcept;
		void BlendWeightCallback(const Support::ParamVariant& p) noexcept;
		void ReloadShader() noexcept;

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbTAA) / sizeof(DWORD);

		RpObjects s_rpObjs;

		inline static constexpr const char* COMPILED_CS[] = { "TAA_cs.cso" };

		// ping-pong between input & output
		Core::Texture m_antiAliased[2];
		uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT];

		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;

		// local constant buffer cache
		cbTAA m_localCB;

		enum class DESC_TABLE
		{
			TEX_A_SRV,
			TEX_A_UAV,
			TEX_B_SRV,
			TEX_B_UAV,
			COUNT
		};

		Core::DescriptorTable m_descTable;
		bool m_isTemporalTexValid;

		struct DefaultParamVals
		{
			static constexpr float BlendWeight = 0.1f;
		};
	};
}