#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct FSR2Pass
	{
		enum class SHADER_IN_RES
		{
			COLOR,
			DEPTH,
			MOTION_VECTOR,
			EXPOSURE,
			COUNT
		};

		enum class SHADER_OUT_RES
		{
			UPSCALED,
			COUNT
		};

		FSR2Pass() = default;
		~FSR2Pass();

		FSR2Pass(FSR2Pass&&) = delete;
		FSR2Pass& operator=(FSR2Pass&&) = delete;

		void Init();
		bool IsInitialized() { return m_initialized; }
		void Activate();
		void OnWindowResized();
		void SetInput(SHADER_IN_RES i, ID3D12Resource* res)
		{
			Assert((int)i < (int)SHADER_IN_RES::COUNT, "out-of-bound access.");

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
			case SHADER_IN_RES::EXPOSURE:
				m_inputResources[(int)SHADER_IN_RES::EXPOSURE] = res;
				break;
			default:
				break;
			}
		}
		const Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES res);

		void Reset();
		void Render(Core::CommandList& cmdList);

	private:
		ID3D12Resource* m_inputResources[(int)SHADER_IN_RES::COUNT] = { 0 };
		uint16_t m_displayWidth = 0;
		uint16_t m_displayHeight = 0;
		bool m_initialized = false;
	};
}