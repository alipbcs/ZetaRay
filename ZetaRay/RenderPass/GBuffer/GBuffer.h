#pragma once

#include "../RenderPass.h"
#include "../../Core/RootSignature.h"
#include "../../Core/GpuMemory.h"
#include "../../Math/Matrix.h"
#include "GBuffer_Common.h"
#include "../../Utility/SmallVector.h"

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
			RTV,
			DEPTH_BUFFER,
			COUNT
		};

		struct InstanceData
		{
			uint64_t InstanceID;
			Math::float4x3 PrevToWorld;
			Math::float4x3 CurrToWorld;
			uint32_t VertexCount;
			uint32_t IndexCount;
			uint64_t VBStartOffsetInBytes;
			uint64_t IBStartOffsetInBytes;
			uint32_t IdxInMatBuff;
		};

		GBufferPass() noexcept;
		~GBufferPass() noexcept;

		void Init(D3D12_GRAPHICS_PIPELINE_STATE_DESC&& psoDesc) noexcept;
		void Reset() noexcept;

		bool IsInitialized() { return m_pso != nullptr; }

		void SetInstances(Util::Span<InstanceData> instances) noexcept;
		void SetDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
		{
			Assert(i < SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_descriptors[i] = h;
		}

		void Render(Core::CommandList& cmdList) noexcept;

	private:
		static constexpr int NUM_CBV = 2;
		static constexpr int NUM_SRV = 1;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 2;
		static constexpr int NUM_CONSTS = 0;

		inline static RpObjects s_rpObjs;

		enum class SHADER_OUT
		{
			GBUFFER_BASECOLOR,
			GBUFFER_NORMAL,
			GBUFFER_METALLIC_ROUGHNESS,
			GBUFFER_MOTION_VECTOR,
			GBUFFER_EMISSIVE,
			GBUFFER_DEPTH,
			COUNT
		};

		struct DrawCallArgs
		{
			uint64_t InstanceID;
			uint64_t VBStartOffsetInBytes;
			uint64_t IBStartOffsetInBytes;
			uint32_t VertexCount;
			uint32_t IndexCount;
		};

		Core::RootSignature m_rootSig;

		// per-draw arguments for the drawcalls
		Util::SmallVector<DrawCallArgs, App::PoolAllocator> m_perDrawCallArgs;

		// constant buffer containing all the Per-draw data
		Core::UploadHeapBuffer m_perDrawCB;

		// cache
		D3D12_CPU_DESCRIPTOR_HANDLE m_descriptors[SHADER_IN_DESC::COUNT] = { 0 };
		ID3D12PipelineState* m_pso = nullptr;

		// TODO: instead of fixed path, get the assets directory from the app
		inline static const char* COMPILED_VS[] = { "GBuffer_vs.cso" };
		inline static const char* COMPILED_PS[] = { "GBuffer_ps.cso" };
	};
}