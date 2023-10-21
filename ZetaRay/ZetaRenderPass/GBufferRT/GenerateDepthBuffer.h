#pragma once

#include "../RenderPass.h"
#include <Core/GpuMemory.h>
#include <Core/RootSignature.h>
#include <Core/DescriptorHeap.h>

namespace ZetaRay::Core
{
	class ComputeCmdList;
}

namespace ZetaRay::RenderPass
{
	struct GenerateRasterDepth
	{
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 0;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = 1;

		GenerateRasterDepth();
		~GenerateRasterDepth();

		GenerateRasterDepth(GenerateRasterDepth&&) = delete;
		GenerateRasterDepth& operator=(GenerateRasterDepth&&) = delete;

		void Resize(uint32_t w, uint32_t h);
		void Render(Core::ComputeCmdList& computeCmdList);

		inline static constexpr const char* COMPILED_CS = "GenerateDepthBuffer_cs.cso";

		RpObjects s_rpObjs;
		Core::RootSignature m_rootSig;
		ID3D12PipelineState* m_pso = nullptr;
		Core::GpuMemory::Texture m_depthBuffer;
		Core::DescriptorTable m_descTable;
	};
}