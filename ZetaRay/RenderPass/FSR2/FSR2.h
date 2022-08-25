#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct FSR2Pass final
	{
		enum class SHADER_IN_RES
		{
			COLOR,
			DEPTH,
			MOTION_VECTOR,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			UPSCALED,
			COUNT
		};

		FSR2Pass() noexcept = default;
		~FSR2Pass() noexcept;

		void Init() noexcept;
		bool IsInitialized() noexcept;
		void OnWindowResized() noexcept;
		void SetInput(SHADER_IN_RES i, ID3D12Resource* res) noexcept
		{
			Assert((int)i < (int)SHADER_IN_RES::COUNT, "out-of-bound access");

			switch (i)
			{
			case SHADER_IN_RES::COLOR:
				m_inputResources[(int)SHADER_IN_RES::COLOR] = res;
				break;
			case SHADER_IN_RES::DEPTH:
				m_inputResources[(int)SHADER_IN_RES::DEPTH] = res;
				break;
			case SHADER_IN_RES::MOTION_VECTOR:
				m_inputResources[(int)SHADER_IN_RES::MOTION_VECTOR] = res;
				break;
			default:
				break;
			}
		}
		const Core::Texture& GetOutput(SHADER_OUT_RES res) noexcept;

		void Reset() noexcept;
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr DXGI_FORMAT UPSCALED_RES_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

		ID3D12Resource* m_inputResources[(int)SHADER_IN_RES::COUNT] = { 0 };
	};
}