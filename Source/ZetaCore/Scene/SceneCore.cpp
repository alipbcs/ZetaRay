#include "SceneCore.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Quaternion.h"
#include "../Math/Color.h"
#include "../RayTracing/RtCommon.h"
#include "../Support/Task.h"
#include "Camera.h"
#include <App/Timer.h>
#include <App/Log.h>
#include <Support/Param.h>
#include <algorithm>

using namespace ZetaRay::Core;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Scene::Internal;
using namespace ZetaRay::Model;
using namespace ZetaRay::Model::glTF;
using namespace ZetaRay::Model::glTF::Asset;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::App;

namespace
{
    uint3 Pcg3d(uint3 v)
    {
        v = v * 1664525u + 1013904223u;
        v.x += v.y * v.z;
        v.y += v.z * v.x;
        v.z += v.x * v.y;
        v = v ^ (v >> 16u);
        v.x += v.y * v.z;
        v.y += v.z * v.x;
        v.z += v.x * v.y;
        return v;
    }
}

//--------------------------------------------------------------------------------------
// Scene
//--------------------------------------------------------------------------------------

SceneCore::SceneCore()
    : m_baseColorDescTable(BASE_COLOR_DESC_TABLE_SIZE),
    m_normalDescTable(NORMAL_DESC_TABLE_SIZE),
    m_metallicRoughnessDescTable(METALLIC_ROUGHNESS_DESC_TABLE_SIZE),
    m_emissiveDescTable(EMISSIVE_DESC_TABLE_SIZE),
    m_sceneGraph(m_memoryPool, m_memoryPool),
    m_prevToWorlds(m_memoryPool),
    m_keyframes(m_memoryPool),
    m_animationMetadata(m_memoryPool)
{}

void SceneCore::Init(Renderer::Interface& rendererInterface)
{
    m_rendererInterface = rendererInterface;
    Assert(m_rendererInterface.Init, "Init() was null.");
    Assert(m_rendererInterface.Update, "Update() was null.");
    Assert(m_rendererInterface.Render, "Render() was null.");
    Assert(m_rendererInterface.Shutdown, "Shutdown() was null.");
    Assert(m_rendererInterface.OnWindowSizeChanged, "OnWindowSizeChanged() was null.");
    Assert(m_rendererInterface.DebugDrawRenderGraph, "DebugDrawRenderGraph() was null.");

    // Level 0 is just a (dummy) root
    m_sceneGraph.reserve(2);

    m_sceneGraph.emplace_back(m_memoryPool);
    m_sceneGraph.emplace_back(m_memoryPool);

    m_sceneGraph[0].m_toWorlds.resize(1);
    m_sceneGraph[0].m_subtreeRanges.resize(1);
    m_sceneGraph[0].m_subtreeRanges[0] = Range(0, 0);

    v_float4x4 I = identity();
    m_sceneGraph[0].m_toWorlds[0] = float4x3(store(I));

    m_baseColorDescTable.Init(XXH3_64bits(GlobalResource::BASE_COLOR_DESCRIPTOR_TABLE,
        strlen(GlobalResource::BASE_COLOR_DESCRIPTOR_TABLE)));
    m_normalDescTable.Init(XXH3_64bits(GlobalResource::NORMAL_DESCRIPTOR_TABLE, strlen(GlobalResource::NORMAL_DESCRIPTOR_TABLE)));
    m_metallicRoughnessDescTable.Init(XXH3_64bits(GlobalResource::METALLIC_ROUGHNESS_DESCRIPTOR_TABLE,
        strlen(GlobalResource::METALLIC_ROUGHNESS_DESCRIPTOR_TABLE)));
    m_emissiveDescTable.Init(XXH3_64bits(GlobalResource::EMISSIVE_DESCRIPTOR_TABLE, strlen(GlobalResource::EMISSIVE_DESCRIPTOR_TABLE)));

    m_rendererInterface.Init();

    // Allocate a slot for the default material
    m_matBuffer.ResizeAdditionalMaterials(1);

    Material defaultMat;
    m_matBuffer.Add(defaultMat, DEFAULT_MATERIAL_IDX);

    ParamVariant animation;
    animation.InitBool("Scene", "Animation", "Pause",
        fastdelegate::MakeDelegate(this, &SceneCore::AnimateCallback),
        !m_animate);
    App::AddParam(animation);
}

void SceneCore::OnWindowSizeChanged()
{
    m_rendererInterface.OnWindowSizeChanged();
}

void SceneCore::Update(double dt, TaskSet& sceneTS, TaskSet& sceneRendererTS)
{
    if (m_isPaused)
        return;

    auto updateWorldTransforms = sceneTS.EmplaceTask("Scene::UpdateWorldTransform", [this, dt]()
        {
            SmallVector<BVH::BVHUpdateInput, App::FrameAllocator> toUpdateInstances;

            if (m_animate)
            {
                SmallVector<AnimationUpdate, App::FrameAllocator> animUpdates;
                UpdateAnimations((float)App::GetTimer().GetTotalTime(), animUpdates);
                UpdateLocalTransforms(animUpdates);

                UpdateWorldTransformations(toUpdateInstances);
            }

            if (m_rebuildBVHFlag)
            {
                App::DeltaTimer timer;
                timer.Start();

                RebuildBVH();
                m_rebuildBVHFlag = false;

                timer.End();
                LOG_UI_INFO("BVH built in %u [ms].", (uint32_t)timer.DeltaMilli());
            }
            else if (!toUpdateInstances.empty())
                m_bvh.Update(toUpdateInstances);
        });

#if 0
    auto frustumCull = sceneTS.EmplaceTask("Scene::FrustumCull", [this]()
        {
            //m_frameInstances.clear();
            m_viewFrustumInstances.free_memory();
            m_viewFrustumInstances.reserve(m_IDtoTreePos.size());

            const Camera& camera = App::GetCamera();
            m_bvh.DoFrustumCulling(camera.GetCameraFrustumViewSpace(), camera.GetViewInv(), m_viewFrustumInstances);

            App::AddFrameStat("Scene", "FrustumCulled", (uint32_t)(m_IDtoTreePos.size() - m_viewFrustumInstances.size()), (uint32_t)m_IDtoTreePos.size());
        });

    sceneTS.AddOutgoingEdge(updateWorldTransforms, frustumCull);
#endif

    const uint32_t numInstances = m_emissives.NumEmissiveInstances();
    m_staleEmissives = false;

    // Full rebuild of the emissive buffers for the first time
    if (numInstances && m_emissives.IsFirstTime())
    {
        const uint32_t numTris = m_emissives.NumEmissiveTriangles();
        m_staleEmissives = true;

        constexpr size_t MAX_NUM_EMISSIVE_WORKERS = 5;
        constexpr size_t MIN_EMISSIVE_INSTANCES_PER_WORKER = 35;
        size_t threadOffsets[MAX_NUM_EMISSIVE_WORKERS];
        size_t threadSizes[MAX_NUM_EMISSIVE_WORKERS];
        uint32_t workerEmissiveCount[MAX_NUM_EMISSIVE_WORKERS];

        const int numEmissiveWorkers = (int)SubdivideRangeWithMin(numInstances,
            MAX_NUM_EMISSIVE_WORKERS,
            threadOffsets,
            threadSizes,
            MIN_EMISSIVE_INSTANCES_PER_WORKER);

        auto finishEmissive = sceneTS.EmplaceTask("BuildEmissiveBuffer", [this]()
            {
                m_emissives.AllocateAndCopyEmissiveBuffer();
            });

        for (int i = 0; i < numEmissiveWorkers; i++)
        {
            StackStr(tname, n, "Scene::Emissive_%d", i);

            auto h = sceneTS.EmplaceTask(tname, [this, offset = threadOffsets[i], size = threadSizes[i]]()
                {
                    auto emissvies = m_emissives.EmissiveInstances();
                    auto tris = m_emissives.EmissiveTriagnles();
                    auto triPos = m_emissives.InitialTriVtxPos();
                    v_float4x4 I = identity();

                    // For every emissive instance, apply world transformation to all of its triangles
                    for (int instance = (int)offset; instance < (int)offset + (int)size; instance++)
                    {
                        auto& e = emissvies[instance];
                        const v_float4x4 vW = load4x3(GetToWorld(e.InstanceID));

                        if (equal(vW, I))
                            continue;

                        const auto rtASInfo = GetInstanceRtASInfo(e.InstanceID);

                        for (int t = e.BaseTriOffset; t < (int)e.BaseTriOffset + (int)e.NumTriangles; t++)
                        {
                            __m128 vV0;
                            __m128 vV1;
                            __m128 vV2;
                            tris[t].LoadVertices(vV0, vV1, vV2);

                            vV0 = mul(vW, vV0);
                            vV1 = mul(vW, vV1);
                            vV2 = mul(vW, vV2);

                            tris[t].StoreVertices(vV0, vV1, vV2);

#if 0
                            triPos[t] = EmissiveBuffer::InitialPos{
                                .Vtx0 = storeFloat3(vV0),
                                .Vtx1 = storeFloat3(vV1),
                                .Vtx2 = storeFloat3(vV2)
                            };
#endif

                            // TODO ID initially contains triangle index within each mesh, after
                            // hashing it below, it's lost and subsequent runs will give wrong results as it
                            // won't match the computation in rt shaders
                            const uint32_t hash = Pcg3d(uint3(rtASInfo.GeometryIndex, rtASInfo.InstanceID, tris[t].ID)).x;

                            Assert(!tris[t].IsIDPatched(), "rewriting emissive triangle ID after the first assignment is invalid.");
                            tris[t].ResetID(hash);
                        }
                    }
                });

            sceneTS.AddOutgoingEdge(updateWorldTransforms, h);
            sceneTS.AddOutgoingEdge(h, finishEmissive);
        }
    }

    if (m_meshBufferStale)
    {
        sceneTS.EmplaceTask("Scene::RebuildMeshBuffers", [this]()
            {
                m_meshes.RebuildBuffers();
            });

        m_meshBufferStale = false;
    }

    m_matBuffer.UpdateGPUBufferIfStale();

    m_rendererInterface.Update(sceneRendererTS);
}

void SceneCore::Shutdown()
{
    m_matBuffer.Clear();
    m_baseColorDescTable.Clear();
    m_normalDescTable.Clear();
    m_metallicRoughnessDescTable.Clear();
    m_emissiveDescTable.Clear();
    m_meshes.Clear();
    m_emissives.Clear();
    m_bvh.Clear();

    m_prevToWorlds.free_memory();
    m_sceneGraph.free_memory();
    m_IDtoTreePos.free();

    m_rendererInterface.Shutdown();
}

uint32_t SceneCore::AddMesh(SmallVector<Vertex>&& vertices, SmallVector<uint32_t>&& indices, 
    uint32_t matIdx, bool lock)
{
    if (lock)
        AcquireSRWLockExclusive(&m_meshLock);

    uint32_t idx = m_meshes.Add(ZetaMove(vertices), ZetaMove(indices), matIdx);

    if (lock)
        ReleaseSRWLockExclusive(&m_meshLock);

    return idx;
}

void SceneCore::AddMeshes(SmallVector<Asset::Mesh>&& meshes, SmallVector<Vertex>&& vertices, 
    SmallVector<uint32_t>&& indices, bool lock)
{
    if(lock)
        AcquireSRWLockExclusive(&m_meshLock);

    m_meshes.AddBatch(ZetaMove(meshes), ZetaMove(vertices), ZetaMove(indices));

    if(lock)
        ReleaseSRWLockExclusive(&m_meshLock);
}

uint32_t SceneCore::AddMaterial(const Asset::MaterialDesc& matDesc, bool lock)
{
    Material mat;
    mat.BaseColorFactor = Float4ToRGBA8(matDesc.BaseColorFactor);
    mat.EmissiveFactorNormalScale = Float4ToRGBA8(float4(matDesc.EmissiveFactor, matDesc.NormalScale));
    mat.MetallicFactorAlphaCuttoff = Float2ToRG8(float2(matDesc.MetallicFactor, matDesc.AlphaCuttoff));
    mat.RoughnessFactor = FloatToUnorm16(matDesc.RoughnessFactor);
    mat.SetAlphaMode(matDesc.AlphaMode);
    mat.SetDoubleSided(matDesc.DoubleSided);

    if (matDesc.Transmission > 0)
    {
        mat.SetIOR(matDesc.IOR);
        mat.SetTransmission(matDesc.Transmission);
    }

    if (lock)
        AcquireSRWLockExclusive(&m_matLock);

    auto idx = m_matBuffer.Add(mat);

    if (lock)
        ReleaseSRWLockExclusive(&m_matLock);

    return idx;
}

void SceneCore::AddMaterial(const Asset::MaterialDesc& matDesc, MutableSpan<Asset::DDSImage> ddsImages,
    bool lock)
{
    Material mat;
    mat.BaseColorFactor = Float4ToRGBA8(matDesc.BaseColorFactor);
    mat.EmissiveFactorNormalScale = Float4ToRGBA8(float4(matDesc.EmissiveFactor, matDesc.NormalScale));
    mat.MetallicFactorAlphaCuttoff = Float2ToRG8(float2(matDesc.MetallicFactor, matDesc.AlphaCuttoff));
    mat.RoughnessFactor = FloatToUnorm16(matDesc.RoughnessFactor);
    mat.SetAlphaMode(matDesc.AlphaMode);
    mat.SetDoubleSided(matDesc.DoubleSided);

    if (matDesc.Transmission > 0)
    {
        mat.SetIOR(matDesc.IOR);
        mat.SetTransmission(matDesc.Transmission);
    }

    auto addTex = [](uint64_t ID, const char* type, TexSRVDescriptorTable& table, uint32_t& tableOffset, MutableSpan<DDSImage> ddsImages)
        {
            auto idx = BinarySearch(Span(ddsImages), ID, [](const DDSImage& obj) {return obj.ID; });
            Check(idx != -1, "%s image with ID %llu was not found.", type, ID);

            tableOffset = table.Add(ZetaMove(ddsImages[idx].T), ID);
        };

    if(lock)
        AcquireSRWLockExclusive(&m_matLock);

    {
        uint32_t tableOffset = Material::INVALID_ID;    // i.e. index in GPU descriptor table

        if (matDesc.BaseColorTexPath != MaterialDesc::INVALID_PATH)
            addTex(matDesc.BaseColorTexPath, "BaseColor", m_baseColorDescTable, tableOffset, ddsImages);

        mat.BaseColorTexture = tableOffset;
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.NormalTexPath != MaterialDesc::INVALID_PATH)
            addTex(matDesc.NormalTexPath, "NormalMap", m_normalDescTable, tableOffset, ddsImages);

        mat.SetNormalTex(tableOffset);
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.MetallicRoughnessTexPath != MaterialDesc::INVALID_PATH)
            addTex(matDesc.MetallicRoughnessTexPath, "MetallicRoughnessMap", m_metallicRoughnessDescTable, tableOffset, ddsImages);

        mat.SetMetallicRoughnessTex(tableOffset);
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.EmissiveTexPath != MaterialDesc::INVALID_PATH)
            addTex(matDesc.EmissiveTexPath, "EmissiveMap", m_emissiveDescTable, tableOffset, ddsImages);

        mat.SetEmissiveTex(tableOffset);
        mat.SetEmissiveStrength(matDesc.EmissiveStrength);
    }

    // Adds material GPU material buffer, which offsets into descriptor tables above.
    // Offset by one to account for default material at slot 0.
    m_matBuffer.Add(mat, matDesc.Index + 1);

    if (lock)
        ReleaseSRWLockExclusive(&m_matLock);
}

void SceneCore::ResizeAdditionalMaterials(uint32_t num)
{
    m_matBuffer.ResizeAdditionalMaterials(num);
}

void SceneCore::AddInstance(Asset::InstanceDesc& instance, bool lock)
{
    const uint64_t meshID = instance.MeshIdx == -1 ? INVALID_MESH : 
        MeshID(instance.MeshIdx, instance.MeshPrimIdx);

    if(lock)
        AcquireSRWLockExclusive(&m_instanceLock);

    if (meshID != INVALID_MESH)
    {
        m_meshBufferStale = true;

        if (instance.RtMeshMode == RT_MESH_MODE::STATIC)
        {
            m_numStaticInstances++;
            m_hasNewStaticInstances = true;
            m_numOpaqueInstances += instance.IsOpaque;
            m_numNonOpaqueInstances += !instance.IsOpaque;
        }
        else
        {
            m_hasNewDynamicInstances = true;
            m_numDynamicInstances++;
        }
    }

    int treeLevel = 1;
    int parentIdx = 0;

    // Get parent's index from the hashmap
    if (instance.ParentID != ROOT_ID)
    {
        const TreePos& p = FindTreePosFromID(instance.ParentID).value();

        treeLevel = p.Level + 1;
        parentIdx = p.Offset;
    }

    const int insertIdx = InsertAtLevel(instance.ID, treeLevel, parentIdx, instance.LocalTransform, meshID,
        instance.RtMeshMode, instance.RtInstanceMask, instance.IsOpaque);

    // Update instance "dictionary"
    {
        Assert(m_IDtoTreePos.find(instance.ID) == nullptr, "instance with id %llu already exists.", instance.ID);
        m_IDtoTreePos.insert_or_assign(instance.ID, TreePos{ .Level = treeLevel, .Offset = insertIdx });

        // Adjust tree positions of shifted instances
        for (int i = insertIdx + 1; i < m_sceneGraph[treeLevel].m_IDs.size(); i++)
        {
            uint64_t insID = m_sceneGraph[treeLevel].m_IDs[i];
            TreePos* p = m_IDtoTreePos.find(insID);
            Assert(p, "instance with ID %llu was not found in the scene graph.", insID);

            // Shift the tree poistion to right
            p->Offset++;
        }
    }

    /*
    // cache the meshe IDs that are emissive
    if (instance.RtInstanceMask & RT_AS_SUBGROUP::EMISSIVE)
    {
        Assert(!instance.Lumen.empty(), "Emissive instances require precomputed per-triangle power");
        m_sceneRenderer.AddEmissiveInstance(instanceID, ZetaMove(instance.Lumen));
    }
    */

    m_rebuildBVHFlag = true;

    if (lock)
        ReleaseSRWLockExclusive(&m_instanceLock);
}

int SceneCore::InsertAtLevel(uint64_t id, int treeLevel, int parentIdx, AffineTransformation& localTransform,
    uint64_t meshID, RT_MESH_MODE rtMeshMode, uint8_t rtInstanceMask, bool isOpaque)
{
    while (treeLevel >= m_sceneGraph.size())
        m_sceneGraph.emplace_back(m_memoryPool);

    auto& parentLevel = m_sceneGraph[treeLevel - 1];
    auto& currLevel = m_sceneGraph[treeLevel];
    auto& parentRange = parentLevel.m_subtreeRanges[parentIdx];

    // Insert position is right next to parent's rightmost child
    const int insertIdx = parentRange.Base + parentRange.Count;

    // Increment parent's children count
    parentRange.Count++;

    // Append it to end, then keep swapping it back until it's at insertIdx
    auto rearrange = []<typename T, typename... Args> requires std::is_swappable<T>::value
        (Vector<T, Support::PoolAllocator>& vec, int insertIdx, Args&&... args)
    {
        vec.emplace_back(T(ZetaForward(args)...));

        for (int i = (int)vec.size() - 1; i != insertIdx; --i)
            std::swap(vec[i], vec[i - 1]);
    };

    float4x3 I = float4x3(store(identity()));

    rearrange(currLevel.m_IDs, insertIdx, id);
    rearrange(currLevel.m_localTransforms, insertIdx, localTransform);
    rearrange(currLevel.m_toWorlds, insertIdx, I);
    rearrange(currLevel.m_meshIDs, insertIdx, meshID);
    const int newBase = currLevel.m_subtreeRanges.empty() ? 0 : currLevel.m_subtreeRanges.back().Base + currLevel.m_subtreeRanges.back().Count;
    rearrange(currLevel.m_subtreeRanges, insertIdx, newBase, 0);
    // Set rebuild flag to true when there's new any instance
    auto flags = SetRtFlags(rtMeshMode, rtInstanceMask, 1, 0, isOpaque);
    rearrange(currLevel.m_rtFlags, insertIdx, flags);
    rearrange(currLevel.m_rtASInfo, insertIdx, RT_AS_Info());

    // Shift base offset of parent's right siblings to right by one
    for (int siblingIdx = parentIdx + 1; siblingIdx != parentLevel.m_subtreeRanges.size(); siblingIdx++)
        parentLevel.m_subtreeRanges[siblingIdx].Base++;

    return insertIdx;
}

void SceneCore::AddAnimation(uint64_t id, MutableSpan<Keyframe> keyframes, float t_start, bool loop, bool isSorted)
{
#ifdef _DEBUG
    TreePos& p = FindTreePosFromID(id).value();
    Assert(GetRtFlags(m_sceneGraph[p.Level].m_rtFlags[p.Offset]).MeshMode != RT_MESH_MODE::STATIC, 
        "Static instances can't be animated.");
#endif

    Check(keyframes.size() > 1, "Invalid animation.");

    if (!isSorted)
    {
        std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const Keyframe& k1, const Keyframe& k2)
            {
                return k1.Time < k2.Time;
            });
    }

    // Remember starting offset and number of keyframes
    const uint32_t currOffset = (uint32_t)m_keyframes.size();
    m_animationMetadata.push_back(AnimationMetadata{
            .InstanceID = id,
            .StartOffset = currOffset,
            .Length = (uint32_t)keyframes.size(),
            .T0 = t_start,
            .Loop = loop
        });

    m_keyframes.append_range(keyframes.begin(), keyframes.end());
}

void SceneCore::AddEmissives(Util::SmallVector<Asset::EmissiveInstance>&& emissiveInstances,
    SmallVector<RT::EmissiveTriangle>&& emissiveTris)
{
    if (emissiveTris.empty())
        return;

    AcquireSRWLockExclusive(&m_emissiveLock);
    m_emissives.AddBatch(ZetaMove(emissiveInstances), ZetaMove(emissiveTris));
    ReleaseSRWLockExclusive(&m_emissiveLock);
}

void SceneCore::RebuildBVH()
{
    SmallVector<BVH::BVHInput, App::FrameAllocator> allInstances;
    allInstances.reserve(m_IDtoTreePos.size());

    const int numLevels = (int)m_sceneGraph.size();
    uint32_t currInsIdx = 0;

    for (int level = 1; level < numLevels; ++level)
    {
        for (int i = 0; i < m_sceneGraph[level].m_toWorlds.size(); ++i)
        {
            // Find the mesh instance
            const uint64_t meshID = m_sceneGraph[level].m_meshIDs[i];
            if (meshID == INVALID_MESH)
                continue;

            const TriangleMesh& mesh = *m_meshes.GetMesh(meshID).value();
            v_AABB vBox(mesh.m_AABB);
            v_float4x4 vM = load4x3(m_sceneGraph[level].m_toWorlds[i]);

            // Transform AABB to world space
            vBox = transform(vM, vBox);
            const uint64_t insID = m_sceneGraph[level].m_IDs[i];

            allInstances.emplace_back(BVH::BVHInput{ .AABB = store(vBox), .InstanceID = insID });
        }
    }

    m_bvh.Build(allInstances);
}

void SceneCore::UpdateWorldTransformations(Vector<BVH::BVHUpdateInput, App::FrameAllocator>& toUpdateInstances)
{
    m_prevToWorlds.clear();
    const int numLevels = (int)m_sceneGraph.size();

    SmallVector<EmissiveInstance, App::FrameAllocator> modifiedEmissives;

    for (int level = 0; level < numLevels - 1; ++level)
    {
        for (int i = 0; i < m_sceneGraph[level].m_subtreeRanges.size(); i++)
        {
            v_float4x4 vParentTransform = load4x3(m_sceneGraph[level].m_toWorlds[i]);
            const auto& range = m_sceneGraph[level].m_subtreeRanges[i];

            for (int j = range.Base; j < range.Base + range.Count; j++)
            {
                AffineTransformation& tr = m_sceneGraph[level + 1].m_localTransforms[j];
                v_float4x4 vLocal = affineTransformation(tr.Scale, tr.Rotation, tr.Translation);
                v_float4x4 newW = mul(vLocal, vParentTransform);
                v_float4x4 prevW = load4x3(m_sceneGraph[level + 1].m_toWorlds[j]);

                if (!m_rebuildBVHFlag && !equal(newW, prevW))
                {
                    const uint64_t ID = m_sceneGraph[level + 1].m_IDs[j];

                    // CPU BVH is currently not needed
#if 0
                    const uint64_t meshID = m_sceneGraph[level + 1].m_meshIDs[j];
                    const TriangleMesh* mesh = m_meshes.GetMesh(meshID).value();
                    v_AABB vOldBox(mesh->m_AABB);
                    vOldBox = transform(prevW, vOldBox);
                    v_AABB vNewBox = transform(newW, vOldBox);

                    toUpdateInstances.emplace_back(BVH::BVHUpdateInput{
                        .OldBox = store(vOldBox),
                        .NewBox = store(vNewBox),
                        .ID = ID });
#endif
                    RT_Flags f = GetRtFlags(m_sceneGraph[level + 1].m_rtFlags[j]);
                    Assert(f.MeshMode != RT_MESH_MODE::STATIC, "Transformation of static meshes can't change.");
                    Assert(!f.RebuildFlag, "Rebuild & update flags can't be set at the same time.");

                    m_sceneGraph[level + 1].m_rtFlags[j] = SetRtFlags(f.MeshMode, f.InstanceMask, 0, 1, f.IsOpaque);

                    auto emissive = m_emissives.FindEmissive(ID);
                    if (emissive)
                        modifiedEmissives.push_back(*emissive.value());
                }

                m_sceneGraph[level + 1].m_toWorlds[j] = float4x3(store(newW));

                m_prevToWorlds.emplace_back(PrevToWorld{
                    .W = m_sceneGraph[level + 1].m_toWorlds[j],
                    .ID = m_sceneGraph[level + 1].m_IDs[j] });
            }
        }
    }

    std::sort(m_prevToWorlds.begin(), m_prevToWorlds.end(),
        [](const PrevToWorld& lhs, const PrevToWorld& rhs)
        {
            return lhs.ID < rhs.ID;
        });

    // TODO implement animated emissives
#if 0
    if (!modifiedEmissives.empty())
    {
        m_staleEmissives = true;
        UpdateEmissives(modifiedEmissives);
    }
#endif
}

void SceneCore::UpdateAnimations(float t, Vector<AnimationUpdate, App::FrameAllocator>& animVec)
{
    for (auto& anim : m_animationMetadata)
    {
        AffineTransformation vRes;

        const Keyframe& kStart = m_keyframes[anim.StartOffset];
        const Keyframe& kEnd = m_keyframes[anim.StartOffset + anim.Length - 1];
        const float t_start = anim.T0;

        // Fast paths
        if (t <= kStart.Time + t_start)
        {
            vRes = kStart.Transform;
        }
        else if (!anim.Loop && t >= kEnd.Time + t_start)
        {
            vRes = kEnd.Transform;
        }
        else 
        {
            if (t >= kEnd.Time + t_start)
            {
                float numLoops = floorf((t - kStart.Time) / (kEnd.Time - kStart.Time));
                float excess = numLoops * (kEnd.Time - kStart.Time) + kStart.Time;
                t -= excess;
                t += kStart.Time;
            }

            auto idx = FindInterval(Span(m_keyframes), t, [](const Keyframe& k) { return k.Time; },
                anim.StartOffset,
                anim.StartOffset + anim.Length - 1);

            Assert(idx != -1, "FindInterval() unexpectedly failed.");
            Keyframe& k1 = m_keyframes[idx];
            Keyframe& k2 = m_keyframes[idx + 1];

            Assert(t >= k1.Time + t_start && t <= k2.Time + t_start, "bug");
            Assert(k1.Time < k2.Time, "divide-by-zero");

            float interpolatedT = (t - (k1.Time + t_start)) / (k2.Time - k1.Time);

            // Scale
            const __m128 vScale1 = loadFloat3(k1.Transform.Scale);
            const __m128 vScale2 = loadFloat3(k2.Transform.Scale);
            const __m128 vScaleInt = lerp(vScale1, vScale2, interpolatedT);

            // Translation
            const __m128 vTranlate1 = loadFloat3(k1.Transform.Translation);
            const __m128 vTranlate2 = loadFloat3(k2.Transform.Translation);
            const __m128 vTranslateInt = lerp(vTranlate1, vTranlate2, interpolatedT);

            // Rotation
            const __m128 vRot1 = loadFloat4(k1.Transform.Rotation);
            const __m128 vRot2 = loadFloat4(k2.Transform.Rotation);
            const __m128 vRotInt = slerp(vRot1, vRot2, interpolatedT);

            vRes.Scale = storeFloat3(vScaleInt);
            vRes.Rotation = storeFloat4(vRotInt);
            vRes.Translation = storeFloat3(vTranslateInt);
        }

        animVec.push_back(AnimationUpdate{
            .M = vRes, 
            .InstanceID = anim.InstanceID });
    }
}

void SceneCore::UpdateLocalTransforms(Span<AnimationUpdate> animVec)
{
    for (auto& update : animVec)
    {
        TreePos t = FindTreePosFromID(update.InstanceID).value();
        m_sceneGraph[t.Level].m_localTransforms[t.Offset] = update.M;
    }
}

// TODO following is untested
void SceneCore::UpdateEmissives(MutableSpan<EmissiveInstance> instances)
{
    auto emissvies = m_emissives.EmissiveInstances();
    auto tris = m_emissives.EmissiveTriagnles();
    auto initPos = m_emissives.InitialTriVtxPos();
    uint32_t minTriIdx = UINT32_MAX;
    uint32_t maxTriIdx = 0;

    for (auto ID : m_toUpdateEmissives)
    {
        auto e = *m_emissives.FindEmissive(ID).value();
        const v_float4x4 vW = load4x3(GetToWorld(ID));

        for (int t = e.BaseTriOffset; t < (int)e.BaseTriOffset + (int)e.NumTriangles; t++)
        {
            __m128 vV0 = loadFloat3(initPos[t].Vtx0);
            __m128 vV1 = loadFloat3(initPos[t].Vtx1);
            __m128 vV2 = loadFloat3(initPos[t].Vtx2);

            vV0 = mul(vW, vV0);
            vV1 = mul(vW, vV1);
            vV2 = mul(vW, vV2);

            tris[t].StoreVertices(vV0, vV1, vV2);
        }

        minTriIdx = Math::Min(minTriIdx, e.BaseTriOffset);
        maxTriIdx = Math::Max(maxTriIdx, e.BaseTriOffset + e.NumTriangles);
    }

    m_emissives.UpdateEmissiveBuffer(minTriIdx, maxTriIdx);
}

void SceneCore::AnimateCallback(const ParamVariant& p)
{
    m_animate = !p.GetBool();
}