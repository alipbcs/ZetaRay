#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Core/DescriptorHeap.h"
#include "../../Math/Matrix.h"
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
			PREV_DEPTH_BUFFER_SRV,
			CURR_DEPTH_BUFFER_DSV,
			COUNT
		};

		GBufferPass() noexcept;
		~GBufferPass() noexcept;

		void Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC&& psoDesc) noexcept;
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
		static constexpr int NUM_CONSTS = (int)std::max(sizeof(cbGBuffer) / sizeof(DWORD), sizeof(cbOcclussionCulling) / sizeof(DWORD));

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
			//DEPTH_PYRAMID,
			OCCLUSION_CULLING,
			COUNT
		};

		enum class DESC_TABLE
		{
			MESH_INSTANCES_SRV,
			INDIRECT_ARGS_UAV,
			COUNT
		};

		Core::DescriptorTable m_gpuDescTable;

		Core::DefaultHeapBuffer m_zeroBuffer;		// for resetting the UAV counter to zero each frame
		Core::DefaultHeapBuffer m_meshInstances;	// frustum-visible meshes in the scene
		Core::DefaultHeapBuffer m_indirectDrawArgs;
		uint32_t m_maxNumDrawCallsSoFar = 0;
		uint32_t m_numMeshesThisFrame = 0;
		uint32_t m_counterBufferOffset = 0;

		Core::RootSignature m_rootSig;
		ComPtr<ID3D12CommandSignature> m_cmdSig;
		D3D12_CPU_DESCRIPTOR_HANDLE m_inputDescriptors[SHADER_IN_DESC::COUNT] = { 0 };

		ID3D12PipelineState* m_computePsos[(int)COMPUTE_SHADERS::COUNT] = { 0 };
		ID3D12PipelineState* m_graphicsPso = nullptr;

		inline static constexpr const char* COMPILED_CS[(int)COMPUTE_SHADERS::COUNT] = {
			//"DepthPyramid.cso",
			"OcclusionCulling_cs.cso"
		};

		inline static constexpr const char* COMPILED_VS[] = { "GBuffer_vs.cso" };
		inline static constexpr const char* COMPILED_PS[] = { "GBuffer_ps.cso" };
	};
}