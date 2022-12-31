#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include <Math/Matrix.h>
#include "GBuffer_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct GBufferPass final
	{
		enum SHADER_IN_DESC
		{
			GBUFFERS_RTV,
			CURR_DEPTH_BUFFER_DSV,
			COUNT
		};

		GBufferPass() noexcept;
		~GBufferPass() noexcept;

		void Init(Util::Span<DXGI_FORMAT> rtvs) noexcept;
		void Reset() noexcept;

		bool IsInitialized() { return m_graphicsPso != nullptr; }

		void SetInstances(Util::Span<MeshInstance> instances) noexcept;
		void SetDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
		{
			Assert(i < SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_inputDescriptors[i] = h;
		}

		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 4;
		static constexpr int NUM_UAV = 1;
		static constexpr int NUM_GLOBS = 5;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbGBuffer) / sizeof(DWORD), sizeof(cbOcclussionCulling) / sizeof(DWORD));

		void CreatePSOs(Util::Span<DXGI_FORMAT> rtvs) noexcept;

		RpObjects s_rpObjs;

		enum class SHADER_OUT
		{
			GBUFFER_BASECOLOR,
			GBUFFER_NORMAL,
			GBUFFER_METALNESS_ROUGHNESS,
			GBUFFER_MOTION_VECTOR,
			GBUFFER_EMISSIVE,
			GBUFFER_DEPTH,
			COUNT
		};

		enum class COMPUTE_SHADERS
		{
			DEPTH_PYRAMID,
			OCCLUSION_CULLING,
			COUNT
		};

		Core::DefaultHeapBuffer m_zeroBuffer;			// for resetting the UAV counter to zero each frame
		Core::DefaultHeapBuffer m_meshInstances;		// frustum-visible meshes in the scene
		Core::DefaultHeapBuffer m_indirectDrawArgs;
		uint32_t m_maxNumDrawCallsSoFar = 0;
		uint32_t m_numMeshesThisFrame = 0;
		uint32_t m_counterSingleSidedBufferOffset = 0;
		uint32_t m_counterDoubleSidedBufferOffset = 0;
		uint32_t m_numSingleSidedMeshes = 0;

		Core::RootSignature m_rootSig;
		ComPtr<ID3D12CommandSignature> m_cmdSig;
		D3D12_CPU_DESCRIPTOR_HANDLE m_inputDescriptors[SHADER_IN_DESC::COUNT] = { 0 };

		enum class PSO
		{
			ONE_SIDED,
			DOUBLE_SIDED,
			COUNT
		};

		ID3D12PipelineState* m_computePsos[(int)COMPUTE_SHADERS::COUNT] = { 0 };
		ID3D12PipelineState* m_graphicsPso[(int)PSO::COUNT] = { 0 };

		inline static constexpr const char* COMPILED_CS[(int)COMPUTE_SHADERS::COUNT] = {
			"DepthPyramid_cs.cso",
			"OcclusionCulling_cs.cso"
		};

		inline static constexpr const char* COMPILED_VS[] = { "GBuffer_vs.cso" };
		inline static constexpr const char* COMPILED_PS[] = { "GBuffer_ps.cso" };
	};
}