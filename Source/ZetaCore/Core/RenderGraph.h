#pragma once

#include "Direct3DUtil.h"
#include "../Utility/Span.h"
#include <FastDelegate/FastDelegate.h>

namespace ZetaRay::Support
{
    struct TaskSet;
    struct WaitObject;
}

namespace ZetaRay::Core
{
    class CommandList;
    class ComputeCmdList;

    enum class RENDER_NODE_TYPE : uint8_t
    {
        RENDER,
        COMPUTE,
        ASYNC_COMPUTE
    };

    struct RenderNodeHandle
    {
        static constexpr int INVALID_HANDLE = -1;

        RenderNodeHandle() = default;
        explicit RenderNodeHandle(int u)
            : Val(u)
        {}

        ZetaInline bool IsValid() const { return Val != INVALID_HANDLE; }

        int Val = INVALID_HANDLE;
    };

    //--------------------------------------------------------------------------------------
    // RenderGraph
    //--------------------------------------------------------------------------------------

    // Workflow:
    // 
    // 1. BeginFrame()
    // 2. All render passes for next frame need to register their resources 
    //    (RenderGraph::RegisterResource()) and themselves (RenderGraph::RegisterRenderPass())
    // 3. MoveToPostRegister()
    // 4. Each render pass calls RenderNode::AddInput() and RenderNode::AddOutput() for 
    //    every resource that it needs along with the expected state. 
    // 5. Barrier
    // 6. Build a DAG based on the resource dependencies
    // 7. Submit command lists to GPU

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

        RenderGraph() = default;
        ~RenderGraph() = default;

        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        void Shutdown();
        void Reset();

        // This should be called at the start of each frame
        void BeginFrame();

        // Adds a node to the graph
        RenderNodeHandle RegisterRenderPass(const char* name, RENDER_NODE_TYPE t, 
            fastdelegate::FastDelegate1<CommandList&> dlg,
            bool forceSeparateCmdList = false);

        // Registers a new resource. This must be called prior to declaring resource 
        // dependencies in each frame.
        void RegisterResource(ID3D12Resource* res, uint64_t path, 
            D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON, 
            bool isWindowSizeDependent = true);

        // Removes given resource (useful for when resources are recreated)
        // Note: these have to be called prior to BeginFrame()
        void RemoveResource(uint64_t path);
        void RemoveResources(Util::Span<uint64_t> paths);

        // Transitions into post-registration. At this point there can be no more Register*() calls.
        void MoveToPostRegister();

        // Adds an input resource to the RenderNodeHandle
        void AddInput(RenderNodeHandle h, uint64_t path, 
            D3D12_RESOURCE_STATES expectedState);

        // Adds an output resource to the RenderNodeHandle
        void AddOutput(RenderNodeHandle h, uint64_t path, 
            D3D12_RESOURCE_STATES expectedState);

        // Builds the graph and submits the rendering tasks with appropriate order
        void Build(Support::TaskSet& ts);

        // Draws the render graph
        void DebugDrawGraph();

        // GPU completion fence for given render node. It must've already been submitted.
        uint64_t GetCompletionFence(RenderNodeHandle h);
        // GPU completion fence for this frame.
        uint64_t GetFrameCompletionFence();

        void SetFrameSubmissionWaitObj(Support::WaitObject& waitObj);

    private:
        static constexpr uint16_t INVALID_NODE_HANDLE = UINT16_MAX;
        static constexpr int MAX_NUM_RENDER_PASSES = 32;
        static constexpr int MAX_NUM_RESOURCES = 64;
        static constexpr int MAX_NUM_PRODUCERS = 5;

        int FindFrameResource(uint64_t key, int beg = 0, int end = -1);
        void BuildTaskGraph(Support::TaskSet& ts);
        void Sort(Util::Span<Util::SmallVector<RenderNodeHandle, App::FrameAllocator>> adjacentTailNodes);
        void InsertResourceBarriers();
        void JoinRenderNodes();
        void MergeSmallNodes();
#ifndef NDEBUG
        void Log();
#endif

        //
        // Frame Resources
        //
        struct ResourceMetadata
        {
            ResourceMetadata() = default;
            ResourceMetadata(const ResourceMetadata& other)
                : ID(other.ID),
                Res(other.Res),
                State(other.State),
                IsWindowSizeDependent(other.IsWindowSizeDependent)
            {
                memcpy(Producers, other.Producers, MAX_NUM_PRODUCERS * sizeof(RenderNodeHandle));
                CurrProdIdx = other.CurrProdIdx.load(std::memory_order_relaxed);
            }
            ResourceMetadata& operator=(const ResourceMetadata& rhs)
            {
                if (this == &rhs)
                    return *this;

                ID = rhs.ID;
                Res = rhs.Res;
                State = rhs.State;
                memcpy(Producers, rhs.Producers, MAX_NUM_PRODUCERS * sizeof(RenderNodeHandle));
                CurrProdIdx = rhs.CurrProdIdx.load(std::memory_order_relaxed);
                IsWindowSizeDependent = rhs.IsWindowSizeDependent;

                return *this;
            }
            void Reset(uint64_t id, ID3D12Resource* r, D3D12_RESOURCE_STATES s, 
                bool isWindowSizeDependent)
            {
                Res = r;
                ID = id;
                IsWindowSizeDependent = isWindowSizeDependent;

                if(State == D3D12_RESOURCE_STATES(-1))
                    State = s;
            }
            void Reset()
            {
                ID = INVALID_ID;
                Res = nullptr;
                CurrProdIdx = 0;
                State = State = D3D12_RESOURCE_STATES(-1);

                for (int i = 0; i < MAX_NUM_PRODUCERS; i++)
                    Producers[i] = RenderNodeHandle(INVALID_NODE_HANDLE);
            }

            static constexpr uint64_t INVALID_ID = UINT64_MAX;

            uint64_t ID = INVALID_ID;
            ID3D12Resource* Res = nullptr;
            std::atomic_uint16_t CurrProdIdx = 0;
            RenderNodeHandle Producers[MAX_NUM_PRODUCERS] = { RenderNodeHandle(INVALID_NODE_HANDLE) };
            D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATES(-1);
            bool IsWindowSizeDependent = false;
        };

        // Make sure this doesn't get reset between frames as some states carry over to the
        // next frame. Producers should be reset though.
        Util::SmallVector<ResourceMetadata> m_frameResources;
        int m_prevFramesNumResources = 0;
        std::atomic_int32_t m_lastResIdx = 0;
        std::atomic_int32_t m_currRenderPassIdx = 0;
        bool m_inBeginEndBlock = false;
        bool m_inPreRegister = false;

        //
        // Nodes
        //
        struct Dependency
        {
            Dependency() = default;
            Dependency(uint64_t id, D3D12_RESOURCE_STATES s)
                : ResID(id), ExpectedState(s)
            {}

            static constexpr uint64_t INVALID_RES_ID = UINT64_MAX;
            uint64_t ResID = INVALID_RES_ID;
            D3D12_RESOURCE_STATES ExpectedState;
        };

        struct RenderNode
        {
            void Reset()
            {
                Inputs.free_memory();
                Outputs.free_memory();
                Barriers.free_memory();
#if 0
                Indegree = 0;
                NodeBatchIdx = -1;
                HasUnsupportedBarrier = false;
                GpuDepSourceIdx = RenderNodeHandle(-1);
                OutputMask = 0;
                memset(Name, 0, MAX_NAME_LENGTH);
                AggNodeIdx = -1;
                ForceSeparateCmdList = false;
#endif
            }

            void Reset(const char* name, RENDER_NODE_TYPE t, fastdelegate::FastDelegate1<CommandList&>& dlg, 
                bool forceSeparateCmdList)
            {
                Type = t;
                Dlg = dlg;
                Indegree = 0;
                NodeBatchIdx = -1;
                Inputs.free_memory();
                Outputs.free_memory();
                Barriers.free_memory();
                HasUnsupportedBarrier = false;
                GpuDepSourceIdx = RenderNodeHandle(-1);
                OutputMask = 0;
                AggNodeIdx = -1;
                ForceSeparateCmdList = forceSeparateCmdList;

                const int n = Math::Min((int)strlen(name), MAX_NAME_LENGTH - 1);
                memcpy(Name, name, n);
                Name[n] = '\0';
            }

            static constexpr int MAX_NAME_LENGTH = 16;

            fastdelegate::FastDelegate1<CommandList&> Dlg;
            int NodeBatchIdx = -1;
            RENDER_NODE_TYPE Type;
            bool HasUnsupportedBarrier = false;
            char Name[MAX_NAME_LENGTH];
            // At most one GPU dependency
            RenderNodeHandle GpuDepSourceIdx = RenderNodeHandle(-1);
            uint32_t OutputMask = 0;
            int16 Indegree = 0;
            int16 AggNodeIdx = -1;
            bool ForceSeparateCmdList = false;

            // Due to usage of FrameAllocator, capacity must be set to zero manually
            // in each frame, otherwise it might reuse previous frame's temp memory.
            Util::SmallVector<Dependency, App::FrameAllocator, 2> Inputs;
            Util::SmallVector<Dependency, App::FrameAllocator, 1> Outputs;
            Util::SmallVector<D3D12_RESOURCE_BARRIER, App::FrameAllocator> Barriers;
        };

        struct AggregateRenderNode
        {
            AggregateRenderNode() = default;
            explicit AggregateRenderNode(bool isAsyncCompute)
                : IsAsyncCompute(isAsyncCompute)
            {}

#if 0
            void Reset()
            {
                Barriers.free_memory();
                Dlgs.free_memory();
                HasUnsupportedBarrier = false;
                CompletionFence = UINT64_MAX;
                GpuDepIdx = RenderNodeHandle(-1);
                TaskH = UINT32_MAX;
                IsAsyncCompute = false;
                IsLast = false;
                ForceSeparate = false;
                memset(Name, 0, MAX_NAME_LENGTH);
            }
#endif

            void Append(const RenderNode& node, int mappedGpeDepIdx, bool forceSeparate = false);

            static constexpr int MAX_NAME_LENGTH = 64;

            Util::SmallVector<D3D12_RESOURCE_BARRIER, App::FrameAllocator, 8> Barriers;
            Util::SmallVector<fastdelegate::FastDelegate1<CommandList&>, App::FrameAllocator, 8> Dlgs;
            uint64_t CompletionFence = UINT64_MAX;
            uint32_t TaskH;
            int BatchIdx = -1;
            int MergedCmdListIdx = -1;
            bool MergeStart = false;
            bool MergeEnd = false;
            // At most one GPU dependency
            RenderNodeHandle GpuDepIdx = RenderNodeHandle(-1);
            char Name[MAX_NAME_LENGTH];
            bool IsAsyncCompute;
            bool HasUnsupportedBarrier = false;
            bool IsLast = false;
            bool ForceSeparate = false;
        };

        static_assert(std::is_move_constructible_v<RenderNode>);
        static_assert(std::is_swappable_v<RenderNode>);

        RenderNode m_renderNodes[MAX_NUM_RENDER_PASSES];
        RenderNodeHandle m_mapping[MAX_NUM_RENDER_PASSES];
        Util::SmallVector<AggregateRenderNode, App::FrameAllocator> m_aggregateNodes;
        Util::SmallVector<ComputeCmdList*, Support::SystemAllocator, 4> m_mergedCmdLists;
        int m_numPassesLastTimeDrawn = -1;
        Support::WaitObject* m_submissionWaitObj = nullptr;
    };
}