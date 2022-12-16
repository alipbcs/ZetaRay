#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "SunShadow_Common.h"

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
		bool IsInitialized() noexcept { return m_psos[(int)SHADERS::SHADOW_MASK] != nullptr; };
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
		static constexpr int NUM_CONSTS = 1;

		RpObjects s_rpObjs;
		Core::RootSignature m_rootSig;

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT SHADOW_MASK = DXGI_FORMAT_R32_UINT;
		};

		Core::Texture m_shadowMask;
		Core::DescriptorTable m_descTable;

		enum class SHADERS
		{
			SHADOW_MASK,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { 0 };
		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = {
			"SunShadow_cs.cso"
		};
	};
}