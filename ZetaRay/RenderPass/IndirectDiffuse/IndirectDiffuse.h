#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "IndirectDiffuse_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct IndirectDiffuse final
	{
		enum class SHADER_OUT_RES
		{
			INDIRECT_LI,
			COUNT
		};

		IndirectDiffuse() noexcept;
		~IndirectDiffuse() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		const Core::Texture& GetOutput(SHADER_OUT_RES i) const noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_out;
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 6;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 7;
		static constexpr int NUM_CONSTS = sizeof(cbIndirectDiffuse) / sizeof(DWORD);

		static const DXGI_FORMAT INDIRECT_LI_TEX_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

		inline static RpObjects s_rpObjs;

		void CreateOutput() noexcept;

		Core::RootSignature m_rootSig;

		// output texture
		Core::Texture m_out;
		Core::DescriptorTable m_outUAV;
		ID3D12PipelineState* m_pso = nullptr;

		inline static const char* COMPILED_CS[] = { "IndirectDiffuse_cs.cso" };

		// shader reload
		void ReloadShaders() noexcept;
	};
}