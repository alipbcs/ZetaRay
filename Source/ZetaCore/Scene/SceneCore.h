#pragma once

#include "../Math/BVH.h"
#include "Asset.h"
#include "SceneRenderer.h"
#include "SceneCommon.h"
#include "../Utility/Utility.h"
#include <xxHash/xxhash.h>

namespace ZetaRay::Model
{
    struct TriangleMesh;
}

namespace ZetaRay::RT
{
    struct StaticBLAS;
    struct TLAS;
}

namespace ZetaRay::Support
{
    struct ParamVariant;
}

namespace ZetaRay::Scene
{
    struct Keyframe
    {
        static Keyframe Identity()
        {
            Keyframe k;
            k.Transform = Math::AffineTransformation::GetIdentity();

            return k;
        }

        Math::AffineTransformation Transform;
        float Time;
    };

    struct RT_Flags
    {
        Model::RT_MESH_MODE MeshMode;
        // Note: Instance masks are specified per instance here, but in DXR can 
        // only be applied per TLAS instance.
        uint8_t InstanceMask;
        bool IsOpaque;
        bool RebuildFlag;
        bool UpdateFlag;
    };

    // 7        6     5         4       3     2     1     0
    //  meshmode    update    build   opaque     instance
    ZetaInline uint8_t SetRtFlags(Model::RT_MESH_MODE m, uint8_t instanceMask, uint8_t rebuild, uint8_t update, bool isOpaque)
    {
        return ((uint8_t)m << 6) | instanceMask | (isOpaque << 3) | (rebuild << 4) | (update << 5);
    }
    ZetaInline RT_Flags GetRtFlags(uint8_t f)
    {
        return RT_Flags{
            .MeshMode = (Model::RT_MESH_MODE)(f >> 6),
            .InstanceMask = (uint8_t)(f & 0x7),
            .IsOpaque = bool((f >> 3) & 0x1),
            .RebuildFlag = bool((f >> 4) & 0x1),
            .UpdateFlag = bool((f >> 5) & 0x1) };
    }

    struct RT_AS_Info
    {
        uint32_t GeometryIndex;
        uint32_t InstanceID;
    };

    class SceneCore
    {
        friend struct RT::StaticBLAS;
        friend struct RT::TLAS;

    public:
        static constexpr uint64_t ROOT_ID = UINT64_MAX;
        static constexpr uint32_t DEFAULT_MATERIAL_IDX = 0;

        static ZetaInline uint64_t InstanceID(uint64_t sceneID, int nodeIdx, int mesh, int meshPrim)
        {
            StackStr(str, n, "instance_%llu_%d_%d_%d", sceneID, nodeIdx, mesh, meshPrim);
            uint64_t instanceFromSceneID = XXH3_64bits(str, n);

            return instanceFromSceneID;
        }

        static ZetaInline uint64_t MeshID(int meshIdx, int meshPrimIdx)
        {
            StackStr(str, n, "mesh_%d_%d", meshIdx, meshPrimIdx);
            uint64_t meshFromSceneID = XXH3_64bits(str, n);

            return meshFromSceneID;
        }

        SceneCore();
        ~SceneCore() = default;

        SceneCore(const SceneCore&) = delete;
        SceneCore& operator=(const SceneCore&) = delete;

        void Init(Renderer::Interface& rendererInterface);
        void Pause() { m_isPaused = true; }
        void Resume() { m_isPaused = false; }
        void OnWindowSizeChanged();
        void Shutdown();

        void Update(double dt, Support::TaskSet& sceneTS, Support::TaskSet& sceneRendererTS);
        void Render(Support::TaskSet& ts) { m_rendererInterface.Render(ts); };

        //
        // Mesh
        //
        uint32_t AddMesh(Util::SmallVector<Core::Vertex>&& vertices, Util::SmallVector<uint32_t>&& indices,
            uint32_t matIdx, bool lock = true);
        void AddMeshes(Util::SmallVector<Model::glTF::Asset::Mesh>&& meshes,
            Util::SmallVector<Core::Vertex>&& vertices,
            Util::SmallVector<uint32_t>&& indices,
            bool lock = true);
        ZetaInline Util::Optional<const Model::TriangleMesh*> GetMesh(uint64_t id) const
        {
            return m_meshes.GetMesh(id);
        }

        ZetaInline const Core::GpuMemory::DefaultHeapBuffer& GetMeshVB() { return m_meshes.GetVB(); }
        ZetaInline const Core::GpuMemory::DefaultHeapBuffer& GetMeshIB() { return m_meshes.GetIB(); }

        //
        // Material
        //
        uint32_t AddMaterial(const Model::glTF::Asset::MaterialDesc& mat, bool lock = true);
        void AddMaterial(const Model::glTF::Asset::MaterialDesc& mat,
            Util::MutableSpan<Model::glTF::Asset::DDSImage> ddsImages, bool lock = true);
        ZetaInline Util::Optional<const Material*> GetMaterial(uint32_t idx) const
        {
            return m_matBuffer.Get(idx);
        }
        void UpdateMaterial(const Material& newMat, int matIdx);
        void ResizeAdditionalMaterials(uint32_t num);

        ZetaInline uint32_t GetBaseColMapsDescHeapOffset() const { return m_baseColorDescTable.GPUDesciptorHeapIndex(); }
        ZetaInline uint32_t GetNormalMapsDescHeapOffset() const { return m_normalDescTable.GPUDesciptorHeapIndex(); }
        ZetaInline uint32_t GetMetallicRougnessMapsDescHeapOffset() const { return m_metallicRoughnessDescTable.GPUDesciptorHeapIndex(); }
        ZetaInline uint32_t GetEmissiveMapsDescHeapOffset() const { return m_emissiveDescTable.GPUDesciptorHeapIndex(); }

        //
        // Instance
        //
        void AddInstance(Model::glTF::Asset::InstanceDesc& instance, bool lock = true);
        ZetaInline Util::Optional<const Math::float4x3*> GetPrevToWorld(uint64_t id) const
        {
            const auto idx = Util::BinarySearch(Util::Span(m_prevToWorlds), id, [](const PrevToWorld& p) {return p.ID; });
            if (idx != -1)
                return &m_prevToWorlds[idx].W;

            return {};
        }
        ZetaInline const Math::float4x3& GetToWorld(uint64_t id) const
        {
            const TreePos& p = FindTreePosFromID(id).value();
            return m_sceneGraph[p.Level].m_toWorlds[p.Offset];
        }
        ZetaInline uint64_t GetInstanceMeshID(uint64_t id) const
        {
            const TreePos& p = FindTreePosFromID(id).value();
            return m_sceneGraph[p.Level].m_meshIDs[p.Offset];
        }
        ZetaInline RT_AS_Info GetInstanceRtASInfo(uint64_t id) const
        {
            const TreePos& p = FindTreePosFromID(id).value();
            return m_sceneGraph[p.Level].m_rtASInfo[p.Offset];
        }
        ZetaInline RT_Flags GetInstanceRtFlags(uint64_t id) const
        {
            const TreePos& p = FindTreePosFromID(id).value();
            return Scene::GetRtFlags(m_sceneGraph[p.Level].m_rtFlags[p.Offset]);
        }
        ZetaInline uint64_t GetIDFromRtMeshIdx(uint32 idx) const
        {
            return m_rtMeshInstanceIdxToID[idx];
        }
        void ReserveInstances(int height, int num);

        //
        // Emissive
        //
        void AddEmissives(Util::SmallVector<Model::glTF::Asset::EmissiveInstance>&& emissiveInstances,
            Util::SmallVector<RT::EmissiveTriangle>&& emissiveTris, bool lock);
        size_t NumEmissiveInstances() const { return m_emissives.NumInstances(); }
        size_t NumEmissiveTriangles() const { return m_emissives.NumTriangles(); }
        bool AreEmissivesStale() const { return m_staleEmissives; }
        void UpdateEmissive(uint64_t instanceID, const Math::float3& emissiveFactor, float strength);

        //
        // Animation
        //
        void AddAnimation(uint64_t id, Util::MutableSpan<Keyframe> keyframes, float t_start,
            bool loop = true, bool isSorted = true);
        void AnimateCallback(const Support::ParamVariant& p);

        //
        // Misc
        //
        ZetaInline Math::AABB GetWorldAABB() { return m_bvh.GetWorldAABB(); }
        ZetaInline uint32_t GetTotalNumInstances() const { return (uint32_t)m_IDtoTreePos.size(); }
        ZetaInline uint32_t GetNumOpaqueInstances() const { return m_numOpaqueInstances; }
        ZetaInline uint32_t GetNumNonOpaqueInstances() const { return m_numNonOpaqueInstances; }
        ZetaInline Util::Span<uint64_t> GetViewFrustumInstances() { return m_viewFrustumInstances; }
        ZetaInline Core::RenderGraph* GetRenderGraph() { return m_rendererInterface.GetRenderGraph(); }
        ZetaInline void DebugDrawRenderGraph() { m_rendererInterface.DebugDrawRenderGraph(); }
        ZetaInline void Pick(uint16 screenPosX, uint16 screenPosY) { m_rendererInterface.Pick(screenPosX, screenPosY); }
        ZetaInline void ClearPick() 
        { 
            m_rendererInterface.ClearPick(); 
            m_pickedInstance.store(Scene::INVALID_INSTANCE, std::memory_order_relaxed);
        }
        ZetaInline void SetPickedInstance(uint64 instanceID) 
        { 
            m_pickedInstance.store(instanceID, std::memory_order_relaxed);
        }
        ZetaInline uint64 GetPickedInstance() { return m_pickedInstance.load(std::memory_order_relaxed); }

    private:
        static constexpr uint32_t BASE_COLOR_DESC_TABLE_SIZE = 256;
        static constexpr uint32_t NORMAL_DESC_TABLE_SIZE = 256;
        static constexpr uint32_t METALLIC_ROUGHNESS_DESC_TABLE_SIZE = 256;
        static constexpr uint32_t EMISSIVE_DESC_TABLE_SIZE = 64;

        struct TreePos
        {
            int Level;
            int Offset;
        };

        struct AnimationUpdate
        {
            Math::AffineTransformation M;
            uint64_t InstanceID;
        };

        struct Range
        {
            Range() = default;
            Range(int b, int c)
                : Base(b),
                Count(c)
            {}

            int Base;
            int Count;
        };

        struct TreeLevel
        {
            Util::SmallVector<uint64_t> m_IDs;
            Util::SmallVector<Math::AffineTransformation> m_localTransforms;
            Util::SmallVector<Math::float4x3> m_toWorlds;
            Util::SmallVector<uint64_t> m_meshIDs;
            Util::SmallVector<Range> m_subtreeRanges;
            Util::SmallVector<uint8_t> m_rtFlags;
            Util::SmallVector<RT_AS_Info> m_rtASInfo;
        };

        struct PrevToWorld
        {
            Math::float4x3 W;
            uint64_t ID;
        };

        // Offset into "m_keyframes" array
        struct AnimationMetadata
        {
            uint64_t InstanceID;
            uint32_t StartOffset;
            uint32_t Length;
            float T0;
            bool Loop;
        };

        ZetaInline Util::Optional<TreePos> FindTreePosFromID(uint64_t id) const
        {
            auto pos = m_IDtoTreePos.find(id);
            if (pos)
                return *pos;

            return {};
        }

        int InsertAtLevel(uint64_t id, int treeLevel, int parentIdx, Math::AffineTransformation& localTransform,
            uint64_t meshID, Model::RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask, bool isOpaque);
        void UpdateWorldTransformations(Util::Vector<Math::BVH::BVHUpdateInput, App::FrameAllocator>& toUpdateInstances);
        void RebuildBVH();
        void UpdateAnimations(float t, Util::Vector<AnimationUpdate, App::FrameAllocator>& animVec);
        void UpdateLocalTransforms(Util::Span<AnimationUpdate> animVec);

        // Maps instance ID to tree position
        Util::HashTable<TreePos> m_IDtoTreePos;
        // Maps RT mesh index to instance ID -- filled in by TLAS::BuildFrameMeshInstanceData()
        Util::SmallVector<uint64> m_rtMeshInstanceIdxToID;
        Util::SmallVector<TreeLevel, Support::SystemAllocator, 3> m_sceneGraph;
        // Previous frame's world transformation
        Util::SmallVector<PrevToWorld> m_prevToWorlds;
        std::atomic_uint64_t m_pickedInstance = Scene::INVALID_INSTANCE;
        bool m_isPaused = false;

        //
        // Scene metadata
        //
        uint32_t m_numStaticInstances = 0;
        uint32_t m_numDynamicInstances = 0;
        uint32_t m_numOpaqueInstances = 0;
        uint32_t m_numNonOpaqueInstances = 0;
        bool m_hasNewStaticInstances = false;
        bool m_hasNewDynamicInstances = false;
        bool m_meshBufferStale = false;
        bool m_dynamicInstanceNeedUpdate = false;
        bool m_rtAsInfoStale = true;

        //
        // BVH
        //
        Math::BVH m_bvh;
        bool m_rebuildBVHFlag = false;

        //
        // Instances
        //
        Util::SmallVector<uint64_t, App::FrameAllocator> m_viewFrustumInstances;

        //
        // Assets
        //
        Internal::MeshContainer m_meshes;
        Internal::MaterialBuffer m_matBuffer;
        Internal::TexSRVDescriptorTable m_baseColorDescTable;
        Internal::TexSRVDescriptorTable m_normalDescTable;
        Internal::TexSRVDescriptorTable m_metallicRoughnessDescTable;
        Internal::TexSRVDescriptorTable m_emissiveDescTable;

        //
        // Emissives
        //
        Internal::EmissiveBuffer m_emissives;
        Util::SmallVector<uint64_t, App::FrameAllocator> m_toUpdateEmissives;
        bool m_staleEmissives = false;

        SRWLOCK m_matLock = SRWLOCK_INIT;
        SRWLOCK m_meshLock = SRWLOCK_INIT;
        SRWLOCK m_instanceLock = SRWLOCK_INIT;
        SRWLOCK m_emissiveLock = SRWLOCK_INIT;

        //
        // Animation
        //
        Util::SmallVector<AnimationMetadata> m_animationMetadata;
        Util::SmallVector<Keyframe> m_keyframes;
        bool m_animate = true;

        //
        // Scene Renderer
        //
        Renderer::Interface m_rendererInterface;
    };
}
