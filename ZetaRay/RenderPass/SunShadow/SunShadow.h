#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/DescriptorHeap.h"
#include "../../Core/GpuMemory.h"
#include "SunShadow_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct SunShadow final
	{
		enum class SHADER_OUT_RES
		{
			SHADOW_MASK,
			COUNT
		};

		SunShadow() noexcept;
		~SunShadow() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept { return m_pso != nullptr; };
		void Reset() noexcept;
		void OnWindowResized() noexcept;
		Core::Texture& GetOutput(SHADER_OUT_RES i) noexcept
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
			return m_shadowMask;
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		void CreateResources() noexcept;
		void ReloadShader() noexcept;

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 4;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 5;
		static constexpr int NUM_CONSTS = sizeof(cbSunShadow) / sizeof(DWORD);

		RpObjects s_rpObjs;
		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;
		Core::Texture m_shadowMask;

		enum class DESC_TABLE
		{
			SHADOW_MASK_UAV,
			COUNT
		};

		Core::DescriptorTable m_descTable;

		inline static constexpr const char* COMPILED_CS[] = { "SunShadow_cs.cso" };
	};
}