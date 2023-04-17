#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include <Core/Constants.h>
#include "GBuffer_Common.h"

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
		void OnWindowResized() noexcept;
		void Update(Util::Span<MeshInstance> instances, ID3D12Resource* currDepthBuffer) noexcept;
		ZetaInline void SetDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
		{
			Assert(i < SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_inputDescriptors[i] = h;
		}
		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 5;
		static constexpr int NUM_UAV = 3;
		static constexpr int NUM_GLOBS = 5;
		static constexpr int NUM_CONSTS = (int)Math::Max(sizeof(cbGBuffer) / sizeof(DWORD), 
			Math::Max(sizeof(cbOcclussionCulling) / sizeof(DWORD), sizeof(cbDepthPyramid) / sizeof(DWORD)));

		void CreatePSOs(Util::Span<DXGI_FORMAT> rtvs) noexcept;
		void CreateDepthPyramid() noexcept;

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
			BUILD_IND_DRAW_ARGS_NO_CULL,
			BUILD_IND_DRAW_ARGS_OCC_CULL,
			COUNT
		};

		Core::DefaultHeapBuffer m_zeroBuffer;			// for resetting the UAV counter to zero each frame
		Core::DefaultHeapBuffer m_meshInstances;		// frustum-visible meshes in the scene in each frame
		Core::DefaultHeapBuffer m_indirectDrawArgs;
		Core::DefaultHeapBuffer m_visibilityBuffer;
		Core::DefaultHeapBuffer m_spdCounter;

		ID3D12Resource* m_currDepthBuffer = nullptr;

		Core::ReadbackHeapBuffer m_readbackBuff;
		ComPtr<ID3D12Fence> m_fence;
		uint64_t m_fenceVals[Core::Constants::NUM_BACK_BUFFERS] = { 0 };
		uint64_t m_nextFenceVal = 1;
		int m_currFrameIdx = 0;				// in [0, NUM_BACK_BUFFERS]
		int m_nextCompletedFrameIdx = 0;	// in [0, NUM_BACK_BUFFERS]
		uint32_t m_lastNumDrawCallsSubmitted = 0;

		enum class DESC_TABLE
		{
			MIP_0_UAV,
			MIP_1_UAV,
			MIP_2_UAV,
			MIP_3_UAV,
			MIP_4_UAV,
			MIP_5_UAV,
			MIP_6_UAV,
			MIP_7_UAV,
			MIP_8_UAV,
			MIP_9_UAV,
			MIP_10_UAV,
			MIP_11_UAV,
			SRV_ALL,
			COUNT
		};

		static constexpr int MAX_NUM_MIPS = 12;
		Core::Texture m_depthPyramid;
		Core::DescriptorTable m_descTable;
		int m_numMips;
		uint32_t m_depthPyramidMip0DimX;
		uint32_t m_depthPyramidMip0DimY;

		uint32_t m_maxNumDrawCallsSoFar = 0;
		uint32_t m_numMeshesThisFrame = 0;
		uint32_t m_lastNumMeshes = 0;
		uint32_t m_counterSingleSidedBufferOffsetFirst = 0;
		uint32_t m_counterDoubleSidedBufferOffsetFirst = 0;
		uint32_t m_counterSingleSidedBufferOffsetSecond = 0;
		uint32_t m_counterDoubleSidedBufferOffsetSecond = 0;
		uint32_t m_numSingleSidedMeshes = 0;
		float m_occlusionTestDepthThresh;

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
			"BuildDrawIndArgs_NoCull_cs.cso",
			"BuildDrawIndArgs_OcclusionCull_cs.cso"
		};

		inline static constexpr const char* COMPILED_VS[] = { "GBuffer_vs.cso" };
		inline static constexpr const char* COMPILED_PS[] = { "GBuffer_ps.cso" };

		void ReloadShader() noexcept;

		struct DefaultParamVals
		{
			static constexpr float DepthThresh = 8e-3f;
		};

		void DepthThreshCallback(const Support::ParamVariant& p) noexcept;
	};
}