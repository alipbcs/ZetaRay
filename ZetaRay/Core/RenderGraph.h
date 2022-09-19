#pragma once

#include "GpuMemory.h"
#include "../Utility/Span.h"
#include <FastDelegate/FastDelegate.h>

namespace ZetaRay::Core
{
	class CommandList;

	enum class RENDER_NODE_TYPE
	{
		RENDER,
		COMPUTE,
		ASYNC_COMPUTE
	};

	struct RenderNodeHandle
	{
		RenderNodeHandle() = default;
		explicit RenderNodeHandle(int u)
			: Val(u)
		{}

		inline bool IsValid() { return Val != -1; }

		int Val = -1;
	};

	//--------------------------------------------------------------------------------------
	// RenderGraph
	//--------------------------------------------------------------------------------------

	// Workflow:
	// 0. BeginFrame()
	// 1. All the render passes for next frame need to register their resources (RenderGraph::RegisterResource())
	// and themselves (RenderGraph::RegisterRenderPass())
	// 2. MoveToPostRegister()
	// 3. Each render pass calls RenderNode::AddInput() and RenderNode::AddOutput() for 
	// every resource R that it needs with the expected state. 
	//		- AddInput adds hash(R) to the node's inputs
	//		- AddOutput adds hash(R) to the node's outputs. Furthermore, for each of 
	//		those reources, corresponding RENDERNODE is designated as the producer of that resource.
	//		This assumes that every resource is produced by exactly one RenderNode.
	// 4. Barrier
	// 5. Create the edges of the graph based on the resource dependencies (by 
	// populating m_adjacentTailNodes for each)
	// 6. BuildAndSubmit

	class RenderGraph
	{
	public:

		enum DUMMY_RES : uint64_t
		{
			RES_0 = 0,
			RES_1,
			RES_2,
			RES_3,
			COUNT
		};

		RenderGraph() noexcept = default;
		~RenderGraph() noexcept = default;

		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		void Shutdown() noexcept;
		void Reset() noexcept;

		// This should be called at the start of each frame
		void BeginFrame() noexcept;

		// Adds a node to the graph
		RenderNodeHandle RegisterRenderPass(const char* name, RENDER_NODE_TYPE t, fastdelegate::FastDelegate1<CommandList&> dlg) noexcept;

		// Registers a new resource. This must be called prior to declaring resource dependencies in each frame
		void RegisterResource(ID3D12Resource* res, uint64_t path, D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON, 
			bool isWindowSizeDependant = true) noexcept;
		
		// Removes given resource (useful for when resources are recreated)
		void RemoveResource(uint64_t path) noexcept;

		// Transitions into post-registration. At this point there can be no more Register*() calls
		void MoveToPostRegister() noexcept;

		// Adds an input resource to the node RenderNodeHandle
		void AddInput(RenderNodeHandle h, uint64_t path, D3D12_RESOURCE_STATES expectedState) noexcept;

		// Adds an output resource to the RenderNodeHandle. It's assumed that each resource
		// can be produced by at most one node.
		void AddOutput(RenderNodeHandle h, uint64_t path, D3D12_RESOURCE_STATES expectedState) noexcept;

		// Builds the graph and submits the rendering tasks with appropriate order
		void Build(Support::TaskSet& ts) noexcept;

		// Draws the render graph
		void DebugDrawGraph() noexcept;

	private:
		static constexpr uint16_t INVALID_NODE_HANDLE = -1;
		static constexpr int MAX_NUM_RENDER_PASSES = 32;
		static constexpr int MAX_NUM_RESOURCES = 64;
		static constexpr int MAX_NUM_PRODUCERS = 5;

		int FindFrameResource(uint64_t key, int beg = 0, int end = -1) noexcept;
		void BuildTaskGraph(Support::TaskSet& ts) noexcept;
		void Sort(Util::Span<Util::SmallVector<RenderNodeHandle>> adjacentTailNodes, Util::Span<RenderNodeHandle> mapping) noexcept;
		void InsertResourceBarriers(Util::Span<RenderNodeHandle> mapping) noexcept;

#ifdef _DEBUG
		void Log() noexcept;
#endif // _DEBUG

		//
		// Frame Resources
		//

		struct ResourceMetadata
		{
			ResourceMetadata() noexcept = default;
			ResourceMetadata(const ResourceMetadata& other) noexcept
			{
				ID = other.ID;
				Res = other.Res;
				State = other.State;
				memcpy(Producers, other.Producers, MAX_NUM_PRODUCERS * sizeof(RenderNodeHandle));
				CurrProdIdx = other.CurrProdIdx.load(std::memory_order_relaxed);
				IsWindowSizeDependant = other.IsWindowSizeDependant;
			}
			ResourceMetadata& operator=(const ResourceMetadata& other) noexcept
			{
				ID = other.ID;
				Res = other.Res;
				State = other.State;
				memcpy(Producers, other.Producers, MAX_NUM_PRODUCERS * sizeof(RenderNodeHandle));
				CurrProdIdx = other.CurrProdIdx.load(std::memory_order_relaxed);
				IsWindowSizeDependant = other.IsWindowSizeDependant;

				return *this;
			}

			void Reset(uint64_t id, ID3D12Resource* r, D3D12_RESOURCE_STATES s, bool isWindowSizeDependant) noexcept
			{
				Res = r;
				ID = id;
				IsWindowSizeDependant = isWindowSizeDependant;

				if(State == D3D12_RESOURCE_STATES(-1))
					State = s;
			}

			void Reset() noexcept
			{
				ID = -1;
				Res = nullptr;
				CurrProdIdx = 0;
				State = State = D3D12_RESOURCE_STATES(-1);
				
				for (int i = 0; i < MAX_NUM_PRODUCERS; i++)
					Producers[i] = RenderNodeHandle(INVALID_NODE_HANDLE);
			}

			uint64_t ID = -1;
			ID3D12Resource* Res = nullptr;
			std::atomic_uint16_t CurrProdIdx = 0;
			RenderNodeHandle Producers[MAX_NUM_PRODUCERS] = { RenderNodeHandle(INVALID_NODE_HANDLE) };
			D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATES(-1);
			bool IsWindowSizeDependant = false;
		};

		// make sure this doesn't get reset between frames as some states carry over to the
		// next frame. Producers should be reset though
		Util::SmallVector<ResourceMetadata> m_frameResources;
		int m_prevFramesNumResources = 0;

		std::atomic_int32_t m_currResIdx = 0;
		std::atomic_int32_t m_currRenderPassIdx = 0;

		//
		// Nodes
		//
		
		struct Dependency
		{
			Dependency() = default;
			Dependency(uint64_t id, D3D12_RESOURCE_STATES s)
				: ResID(id), ExpectedState(s)
			{}

			uint64_t ResID = -1;
			D3D12_RESOURCE_STATES ExpectedState;
		};

		struct RenderNode
		{
			void Reset() noexcept
			{
				Indegree = 0;
				BatchIdx = -1;
				Inputs.clear();
				Outputs.clear();
				Barriers.clear();
				HasUnsupportedBarrier = false;
				CompletionFence = -1;
				GpuDepSourceIdx = RenderNodeHandle(-1);
				TaskH = -1;
				OutputMask = 0;
				memset(Name, 0, MAX_NAME_LENGTH);
			}

			void Reset(const char* name, RENDER_NODE_TYPE t, fastdelegate::FastDelegate1<CommandList&>& dlg) noexcept
			{
				Type = t;
				Dlg = dlg;
				Indegree = 0;
				BatchIdx = -1;
				Inputs.clear();
				Outputs.clear();
				Barriers.clear();
				HasUnsupportedBarrier = false;
				CompletionFence = -1;
				GpuDepSourceIdx = RenderNodeHandle(-1);
				TaskH = -1;
				OutputMask = 0;

				int n = std::min((int)strlen(name), MAX_NAME_LENGTH - 1);
				memcpy(Name, name, n);
				Name[n] = '\0';
			}

			RENDER_NODE_TYPE Type;
			bool HasUnsupportedBarrier = false;
			fastdelegate::FastDelegate1<CommandList&> Dlg;

			static constexpr int MAX_NAME_LENGTH = 16;
			char Name[MAX_NAME_LENGTH];

			Util::SmallVector<Dependency, 2> Inputs;
			Util::SmallVector<Dependency, 1> Outputs;
			Util::SmallVector<D3D12_RESOURCE_BARRIER> Barriers;

			uint32_t OutputMask = 0;
			int Indegree = 0;
			int BatchIdx = -1;

			uint64_t CompletionFence = -1;

			// at most one GPU dependency
			RenderNodeHandle GpuDepSourceIdx = RenderNodeHandle(-1);

			//TaskSet::TaskHandle TaskH;
			uint32_t TaskH;
		};

		static_assert(std::is_move_constructible_v<RenderNode>);
		static_assert(std::is_swappable_v<RenderNode>);

		RenderNode m_renderNodes[MAX_NUM_RENDER_PASSES];

		int m_numPassesPrevFrame;
	};
}