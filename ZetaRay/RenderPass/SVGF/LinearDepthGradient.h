#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "LinearDepthGradient_Common.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct LinearDepthGradient final
		{
			enum SHADER_OUT_RES
			{
				GRADIENT,
				COUNT
			};

			LinearDepthGradient() noexcept;
			~LinearDepthGradient() noexcept;

			void Init() noexcept;
			bool IsInitialized() noexcept { return m_pso != nullptr; };
			void Reset() noexcept;
			void OnWindowResized() noexcept;
			Texture& GetOutput(int i) noexcept
			{
				Assert(i < SHADER_OUT_RES::COUNT, "out-of-bound access.");
				return m_out;
			}
			void Render(CommandList& cmdList) noexcept;

		private:
			void CreateOutput() noexcept;
			void ReloadShader() noexcept;

			static constexpr int NUM_CBV = 1;
			static constexpr int NUM_SRV = 0;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 1;
			static constexpr int NUM_CONSTS = sizeof(cbLinearDepthGrad) / sizeof(DWORD);

			inline static RpObjects s_rpObjs;

			RootSignature m_rootSig;

			// output texture
			Texture m_out;
			DescriptorTable m_outUAV;

			inline static const char* COMPILED_CS[] = { "LinearDepthGradient_cs.cso" };
			ID3D12PipelineState* m_pso = nullptr;
		};
	}
}