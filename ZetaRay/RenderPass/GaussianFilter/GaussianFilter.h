#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "GaussianFilter_Common.h"

namespace ZetaRay
{
	class CommandList;

	namespace RenderPass
	{
		struct GaussianFilter final
		{
			enum class SHADER_IN_DESC
			{
				SIGNAL,
				COUNT
			};

			enum class SHADER_OUT_RES
			{
				FILTERED,
				COUNT
			};

			GaussianFilter() noexcept;
			~GaussianFilter() noexcept;

			// "f" must match the format of resource specified by SHADER_IN_DESC::SIGNAL
			void Init(const char* owner, int inputWidth, int inputHeight, DXGI_FORMAT f) noexcept;
			bool IsInitialized() noexcept { return m_pso != nullptr; };
			void Reset() noexcept;
			void OnResize(int inputWidth, int inputHeight, DXGI_FORMAT f) noexcept;
			void SetDescriptor(int i, uint32_t heapIdx) noexcept
			{
				Assert(i < (int)SHADER_IN_DESC::COUNT, "out-of-bound access.");
				m_inputDesc[i] = heapIdx;
			}
			const Texture& GetOutput(int i) const noexcept
			{
				Assert(i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");
				return m_filtered;
			}
			void Render(CommandList& cmdList) noexcept;

		private:
			void CreateOutput(int inputWidth, int inputHeight, DXGI_FORMAT f) noexcept;

			static constexpr int NUM_CBV = 0;
			static constexpr int NUM_SRV = 0;
			static constexpr int NUM_UAV = 0;
			static constexpr int NUM_GLOBS = 0;
			static constexpr int NUM_CONSTS = sizeof(cbGaussianFilter) / sizeof(DWORD);

			inline static RpObjects s_rpObjs;

			inline static const char* COMPILED_CS[] = { "GaussianFilter_cs.cso" };

			Texture m_filtered;
			DescriptorTable m_descTable;

			uint32_t m_inputDesc[(int)SHADER_IN_DESC::COUNT] = { 0 };

			RootSignature m_rootSig;
			ID3D12PipelineState* m_pso = nullptr;
		};
	}
}