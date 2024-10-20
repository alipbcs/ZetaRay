#include "SceneCore.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Quaternion.h"
#include "../Support/Task.h"
#include "Camera.h"
#include <App/Timer.h>
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
    m_emissiveDescTable(EMISSIVE_DESC_TABLE_SIZE)
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
    //m_sceneGraph.reserve(2);
    //m_sceneGraph.emplace_back(TreeLevel());
    //m_sceneGraph.emplace_back(TreeLevel());
    m_sceneGraph.resize(2);

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
    m_matBuffer.Add(DEFAULT_MATERIAL_ID, defaultMat);

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

    auto updateWorldTransforms = sceneTS.EmplaceTask("Scene::UpdateWorldTransform", [this]()
        {
            if (m_rebuildBVHFlag)
                InitWorldTransformations();

            if (m_animate)
            {
                SmallVector<AnimationUpdate, App::FrameAllocator> animUpdates;
                UpdateAnimations((float)App::GetTimer().GetTotalTime(), animUpdates);
                UpdateLocalTransforms(animUpdates);
            }

            if (!m_instanceUpdates.empty())
            {
                SmallVector<BVH::BVHUpdateInput, App::FrameAllocator> toUpdateInstances;
                UpdateWorldTransformations(toUpdateInstances);
            }

            // TODO BVH is wrong
#if 0
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
#endif
            m_rebuildBVHFlag = false;
        });

    TaskSet::TaskHandle setRtAsInfo = TaskSet::INVALID_TASK_HANDLE;

    // RT-AS info is needed to compute unique hash for emissives
    if (m_rtAsInfoStale)
    {
        setRtAsInfo = sceneTS.EmplaceTask("Scene::UpdateRtAsInfo", [this]()
            {
                // Following must exactly match the iteration order of StaticBLAS::Rebuild().
                uint32_t currInstance = 0;

                for (size_t treeLevelIdx = 1; treeLevelIdx < m_sceneGraph.size(); treeLevelIdx++)
                {
                    auto& currTreeLevel = m_sceneGraph[treeLevelIdx];

                    for (size_t i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
                    {
                        const Scene::RT_Flags flags = RT_Flags::Decode(currTreeLevel.m_rtFlags[i]);

                        if (flags.MeshMode == RT_MESH_MODE::STATIC)
                        {
                            const uint64_t meshID = currTreeLevel.m_meshIDs[i];
                            if (meshID == Scene::INVALID_MESH)
                                continue;

                            currTreeLevel.m_rtASInfo[i] = RT_AS_Info{
                                .GeometryIndex = currInstance,
                                .InstanceID = 0 };

                            currInstance++;
                        }
                    }
                }
            });

        m_rtAsInfoStale = false;
    }

#if 0
    auto frustumCull = sceneTS.EmplaceTask("Scene::FrustumCull", [this]()
        {
            //m_frameInstances.clear();
            m_viewFrustumInstances.free_memory();
            m_viewFrustumInstances.reserve(m_IDtoTreePos.size());

            const Camera& camera = App::GetCamera();
            m_bvh.DoFrustumCulling(camera.GetCameraFrustumViewSpace(), camera.GetViewInv(), m_viewFrustumInstances);

            App::AddFrameStat("Scene", "FrustumCulled", 
                (uint32_t)(m_IDtoTreePos.size() - m_viewFrustumInstances.size()), 
                (uint32_t)m_IDtoTreePos.size());
        });

    sceneTS.AddOutgoingEdge(updateWorldTransforms, frustumCull);
#endif

    const uint32_t numInstances = m_emissives.NumInstances();
    m_staleEmissives = false;

    if (numInstances && (!m_emissives.TransformedToWorldSpace() || m_emissives.HasStaleMaterials()))
    {
        m_staleEmissives = true;

        auto upload = sceneTS.EmplaceTask("UploadEmissiveBuffer", [this]()
            {
                m_emissives.UploadToGPU();
            });

        // Full rebuild of emissive buffer for first time
        if (!m_emissives.TransformedToWorldSpace())
        {
            constexpr size_t MAX_NUM_EMISSIVE_WORKERS = 5;
            constexpr size_t MIN_EMISSIVE_INSTANCES_PER_WORKER = 35;
            size_t threadOffsets[MAX_NUM_EMISSIVE_WORKERS];
            size_t threadSizes[MAX_NUM_EMISSIVE_WORKERS];

            const int numEmissiveWorkers = (int)SubdivideRangeWithMin(numInstances,
                MAX_NUM_EMISSIVE_WORKERS,
                threadOffsets,
                threadSizes,
                MIN_EMISSIVE_INSTANCES_PER_WORKER);

            for (int i = 0; i < numEmissiveWorkers; i++)
            {
                StackStr(tname, n, "Scene::Emissive_%d", i);

                auto h = sceneTS.EmplaceTask(tname, [this, offset = threadOffsets[i], size = threadSizes[i]]()
                    {
                        auto emissvies = m_emissives.Instances();
                        auto tris = m_emissives.Triagnles();
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
                                const uint32_t hash = Pcg3d(uint3(rtASInfo.GeometryIndex, rtASInfo.InstanceID,
                                    tris[t].ID)).x;

                                Assert(!tris[t].IsIDPatched(), "rewriting emissive triangle ID after the first assignment is invalid.");
                                tris[t].ResetID(hash);
                            }
                        }
                    });

                sceneTS.AddOutgoingEdge(updateWorldTransforms, h);

                if (setRtAsInfo != TaskSet::INVALID_TASK_HANDLE)
                    sceneTS.AddOutgoingEdge(setRtAsInfo, h);

                sceneTS.AddOutgoingEdge(h, upload);
            }
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

    m_matBuffer.UploadToGPU();
    m_rendererInterface.Update(sceneRendererTS);
}

void SceneCore::Shutdown()
{
    // Make sure all GPU resources (texture, buffers, etc) are manually released,
    // as they normally call the GPU memory subsystem upon destruction, which
    // is deleted at that point.
    m_matBuffer.Clear();
    m_baseColorDescTable.Clear();
    m_normalDescTable.Clear();
    m_metallicRoughnessDescTable.Clear();
    m_emissiveDescTable.Clear();
    m_meshes.Clear();
    m_emissives.Clear();

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
    if (lock)
        AcquireSRWLockExclusive(&m_meshLock);

    m_meshes.AddBatch(ZetaMove(meshes), ZetaMove(vertices), ZetaMove(indices));

    if (lock)
        ReleaseSRWLockExclusive(&m_meshLock);
}

void SceneCore::AddMaterial(const Asset::MaterialDesc& matDesc, bool lock)
{
    Material mat;
    mat.SetBaseColorFactor(matDesc.BaseColorFactor);
    mat.SetMetallic(matDesc.MetallicFactor);
    mat.SetSpecularRoughness(matDesc.SpecularRoughnessFactor);
    mat.SetSpecularIOR(matDesc.SpecularIOR);
    mat.SetTransmission(matDesc.TransmissionWeight);
    mat.SetSubsurface(matDesc.SubsurfaceWeight);
    mat.SetCoatWeight(matDesc.CoatWeight);
    mat.SetCoatColor(matDesc.CoatColor);
    mat.SetCoatRoughness(matDesc.CoatRoughness);
    mat.SetCoatIOR(matDesc.CoatIOR);
    mat.SetEmissiveFactor(matDesc.EmissiveFactor);
    mat.SetEmissiveStrength(matDesc.EmissiveStrength);
    mat.SetNormalScale(matDesc.NormalScale);
    mat.SetAlphaCutoff(matDesc.AlphaCutoff);
    mat.SetAlphaMode(matDesc.AlphaMode);
    mat.SetDoubleSided(matDesc.DoubleSided);

    if (lock)
        AcquireSRWLockExclusive(&m_matLock);

    m_matBuffer.Add(matDesc.ID, mat);

    if (lock)
        ReleaseSRWLockExclusive(&m_matLock);
}

void SceneCore::AddMaterial(const Asset::MaterialDesc& matDesc, MutableSpan<Asset::DDSImage> ddsImages,
    bool lock)
{
    Material mat;
    mat.SetBaseColorFactor(matDesc.BaseColorFactor);
    mat.SetMetallic(matDesc.MetallicFactor);
    mat.SetSpecularRoughness(matDesc.SpecularRoughnessFactor);
    mat.SetSpecularIOR(matDesc.SpecularIOR);
    mat.SetTransmission(matDesc.TransmissionWeight);
    mat.SetSubsurface(matDesc.SubsurfaceWeight);
    mat.SetCoatWeight(matDesc.CoatWeight);
    mat.SetCoatColor(matDesc.CoatColor);
    mat.SetCoatRoughness(matDesc.CoatRoughness);
    mat.SetCoatIOR(matDesc.CoatIOR);
    mat.SetEmissiveFactor(matDesc.EmissiveFactor);
    mat.SetEmissiveStrength(matDesc.EmissiveStrength);
    mat.SetNormalScale(matDesc.NormalScale);
    mat.SetAlphaCutoff(matDesc.AlphaCutoff);
    mat.SetAlphaMode(matDesc.AlphaMode);
    mat.SetDoubleSided(matDesc.DoubleSided);

    auto addTex = [](uint64_t ID, const char* type, TexSRVDescriptorTable& table, uint32_t& tableOffset, 
        MutableSpan<DDSImage> ddsImages)
        {
            auto idx = BinarySearch(Span(ddsImages), ID, [](const DDSImage& obj) {return obj.ID; });
            Check(idx != -1, "%s image with ID %llu was not found.", type, ID);

            tableOffset = table.Add(ZetaMove(ddsImages[idx].T), ID);
        };

    if (lock)
        AcquireSRWLockExclusive(&m_matLock);

    {
        uint32_t tableOffset = Material::INVALID_ID;    // i.e. index in GPU descriptor table

        if (matDesc.BaseColorTexPath != MaterialDesc::INVALID_PATH)
        {
            addTex(matDesc.BaseColorTexPath, "BaseColor", m_baseColorDescTable, tableOffset, ddsImages);
            mat.SetBaseColorTex(tableOffset);
        }
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.NormalTexPath != MaterialDesc::INVALID_PATH)
        {
            addTex(matDesc.NormalTexPath, "NormalMap", m_normalDescTable, tableOffset, ddsImages);
            mat.SetNormalTex(tableOffset);
        }
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.MetallicRoughnessTexPath != MaterialDesc::INVALID_PATH)
        {
            addTex(matDesc.MetallicRoughnessTexPath, "MetallicRoughnessMap", 
                m_metallicRoughnessDescTable, tableOffset, ddsImages);

            mat.SetMetallicRoughnessTex(tableOffset);
        }
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.EmissiveTexPath != MaterialDesc::INVALID_PATH)
        {
            addTex(matDesc.EmissiveTexPath, "EmissiveMap", m_emissiveDescTable, tableOffset, ddsImages);
            mat.SetEmissiveTex(tableOffset);
        }
    }

    // Add this material to GPU material buffer. Contained texture indices offset into 
    // descriptor tables above.
    m_matBuffer.Add(matDesc.ID, mat);

    if (lock)
        ReleaseSRWLockExclusive(&m_matLock);
}

void SceneCore::UpdateMaterial(uint32 ID, const Material& newMat)
{
    m_matBuffer.Update(ID, newMat);
    m_rendererInterface.SceneModified();
}

void SceneCore::ResizeAdditionalMaterials(uint32_t num)
{
    m_matBuffer.ResizeAdditionalMaterials(num);
}

void SceneCore::AddInstance(Asset::InstanceDesc& instance, bool lock)
{
    const uint64_t meshID = instance.MeshIdx == -1 ? INVALID_MESH :
        MeshID(instance.SceneID, instance.MeshIdx, instance.MeshPrimIdx);

    if (lock)
        AcquireSRWLockExclusive(&m_instanceLock);

    if (meshID != INVALID_MESH)
    {
        m_meshBufferStale = true;

        if (instance.RtMeshMode == RT_MESH_MODE::STATIC)
        {
            m_numStaticInstances++;
            m_numOpaqueInstances += instance.IsOpaque;
            m_numNonOpaqueInstances += !instance.IsOpaque;
        }
        else
            m_numDynamicInstances++;
    }

    uint32_t treeLevel = 1;
    uint32_t parentIdx = 0;

    // Get parent's index from the hashmap
    if (instance.ParentID != ROOT_ID)
    {
        const TreePos& p = FindTreePosFromID(instance.ParentID).value();

        treeLevel = p.Level + 1;
        parentIdx = p.Offset;
    }

    const uint32_t insertIdx = InsertAtLevel(instance.ID, treeLevel, parentIdx, instance.LocalTransform, meshID,
        instance.RtMeshMode, instance.RtInstanceMask, instance.IsOpaque);

    // Update instance "dictionary"
    {
        Assert(!m_IDtoTreePos.find(instance.ID), "instance with id %llu already exists.", instance.ID);
        m_IDtoTreePos.insert_or_assign(instance.ID, TreePos{ .Level = treeLevel, .Offset = insertIdx });

        // Adjust tree positions of shifted instances
        for (size_t i = insertIdx + 1; i < m_sceneGraph[treeLevel].m_IDs.size(); i++)
        {
            uint64_t insID = m_sceneGraph[treeLevel].m_IDs[i];
            auto pos = m_IDtoTreePos.find(insID);

            // Shift tree position to right
            pos.value()->Offset++;
        }
    }

    /*
    // cache emissive mesh IDs
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

uint32_t SceneCore::InsertAtLevel(uint64_t id, uint32_t treeLevel, uint32_t parentIdx, 
    AffineTransformation& localTransform, uint64_t meshID, RT_MESH_MODE rtMeshMode, 
    uint8_t rtInstanceMask, bool isOpaque)
{
    m_sceneGraph.resize(Max(treeLevel + 1, (uint32_t)m_sceneGraph.size()));

    //while (m_sceneGraph.size() <= treeLevel)
    //    m_sceneGraph.emplace_back(TreeLevel());

    auto& parentLevel = m_sceneGraph[treeLevel - 1];
    auto& currLevel = m_sceneGraph[treeLevel];
    auto& parentRange = parentLevel.m_subtreeRanges[parentIdx];

    // Insert position is right next to parent's rightmost child
    const uint32_t insertIdx = parentRange.Base + parentRange.Count;

    // Increment parent's #children
    parentRange.Count++;

    auto rearrange = []<typename T, typename... Args> requires std::is_trivially_copyable_v<T>
        (Vector<T>& vec, uint32_t insertIdx, Args&&... args)
    {
        const size_t numToMove = vec.size() - insertIdx;

        // Resize for one additional entry
        vec.resize(vec.size() + 1);
        // Shift existing elements with index >= insertIdx to right by one
        memmove(vec.data() + insertIdx + 1, vec.data() + insertIdx, numToMove * sizeof(T));

        // Construct the new entry in-place
        new (vec.data() + insertIdx) T(ZetaForward(args)...);
    };

    float4x3 I = float4x3(store(identity()));

    Assert(insertIdx <= currLevel.m_IDs.size(), "Out-of-bounds insertion index.");
    rearrange(currLevel.m_IDs, insertIdx, id);
    rearrange(currLevel.m_localTransforms, insertIdx, localTransform);
    rearrange(currLevel.m_toWorlds, insertIdx, I);
    rearrange(currLevel.m_meshIDs, insertIdx, meshID);
    const uint32_t newBase = currLevel.m_subtreeRanges.empty() ? 0 :
        currLevel.m_subtreeRanges.back().Base + currLevel.m_subtreeRanges.back().Count;
    rearrange(currLevel.m_subtreeRanges, insertIdx, newBase, 0);
    // Set rebuild flag to true when there's new any instance
    auto flags = RT_Flags::Encode(rtMeshMode, rtInstanceMask, 1, 0, isOpaque);
    rearrange(currLevel.m_rtFlags, insertIdx, flags);
    rearrange(currLevel.m_rtASInfo, insertIdx, RT_AS_Info());

    // Shift base offset of parent's right siblings to right by one
    for (size_t siblingIdx = parentIdx + 1; siblingIdx != parentLevel.m_subtreeRanges.size(); siblingIdx++)
        parentLevel.m_subtreeRanges[siblingIdx].Base++;

    return insertIdx;
}

void SceneCore::AddAnimation(uint64_t id, MutableSpan<Keyframe> keyframes, float t_start, 
    bool loop, bool isSorted)
{
#ifndef NDEBUG
    TreePos& p = FindTreePosFromID(id).value();
    Assert(RT_Flags::Decode(m_sceneGraph[p.Level].m_rtFlags[p.Offset]).MeshMode != RT_MESH_MODE::STATIC,
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

void SceneCore::TransformInstance(uint64_t id, const float3& tr, const float3x3& rotation,
    const float3& scale)
{
    m_tempWorldTransformUpdates[id] = TransformUpdate{ .Tr = tr, .Rotation = rotation, .Scale = scale };

    ConvertInstanceDynamic(id);
    // Updates if instance already exists
    m_instanceUpdates[id] = App::GetTimer().GetTotalFrameCount();

    m_rendererInterface.SceneModified();
}

void SceneCore::ReserveInstances(int height, int num)
{
    // +1 for root
    m_sceneGraph.reserve(height + 1);
    m_prevToWorlds.resize(num, true);
    m_IDtoTreePos.resize(num, true);
    m_worldTransformUpdates.resize(Min(num, 32), true);
}

void SceneCore::AddEmissives(Util::SmallVector<Asset::EmissiveInstance>&& emissiveInstances,
    SmallVector<RT::EmissiveTriangle>&& emissiveTris, bool lock)
{
    if (emissiveTris.empty())
        return;

    if(lock)
        AcquireSRWLockExclusive(&m_emissiveLock);
    
    m_emissives.AddBatch(ZetaMove(emissiveInstances), ZetaMove(emissiveTris));

    if(lock)
        ReleaseSRWLockExclusive(&m_emissiveLock);
}

void SceneCore::UpdateEmissive(uint64_t instanceID, const float3& emissiveFactor, float strength)
{
    m_emissives.Update(instanceID, emissiveFactor, strength);
    m_rendererInterface.SceneModified();
}

// Currently, not needed
#if 0
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

            allInstances.emplace_back(BVH::BVHInput{ .BoundingBox = store(vBox), .InstanceID = insID });
        }
    }

    m_bvh.Build(allInstances);
}
#endif

void SceneCore::InitWorldTransformations()
{
    const float4x3 I = float4x3(store(identity()));

    // No parent transformation for first level
    for (size_t i = 0; i < m_sceneGraph[1].m_localTransforms.size(); i++)
    {
        AffineTransformation& tr = m_sceneGraph[1].m_localTransforms[i];
        v_float4x4 vLocal = affineTransformation(tr.Scale, tr.Rotation, tr.Translation);
        const uint64_t ID = m_sceneGraph[1].m_IDs[i];

        // Set prev = new for 1st frame
        m_sceneGraph[1].m_toWorlds[i] = float4x3(store(vLocal));
        m_prevToWorlds[ID] = m_sceneGraph[1].m_toWorlds[i];
    }

    const size_t numLevels = m_sceneGraph.size();

    for (size_t level = 1; level < numLevels - 1; ++level)
    {
        for (size_t i = 0; i < m_sceneGraph[level].m_subtreeRanges.size(); i++)
        {
            const v_float4x4 vParentTr = load4x3(m_sceneGraph[level].m_toWorlds[i]);
            const float4x3 P = float4x3(store(vParentTr));
            const auto& range = m_sceneGraph[level].m_subtreeRanges[i];

            for (size_t j = range.Base; j < range.Base + range.Count; j++)
            {
                AffineTransformation& tr = m_sceneGraph[level + 1].m_localTransforms[j];
                v_float4x4 vLocal = affineTransformation(tr.Scale, tr.Rotation, tr.Translation);
                // Bottom up transformation hierarchy
                v_float4x4 newW = mul(vLocal, vParentTr);
                const uint64_t ID = m_sceneGraph[level + 1].m_IDs[j];

                // Set prev = new for 1st frame
                m_sceneGraph[level + 1].m_toWorlds[j] = float4x3(store(newW));
                m_prevToWorlds[ID] = m_sceneGraph[level + 1].m_toWorlds[j];
            }
        }
    }
}

void SceneCore::UpdateWorldTransformations(Vector<BVH::BVHUpdateInput, App::FrameAllocator>& toUpdateInstances)
{
    struct Entry
    {
        v_float4x4 W;
        uint32_t TreeLevel;
        uint32_t Base;
        uint32_t Count;
    };

    SmallVector<Entry, App::FrameAllocator, 10> stack;
    const auto currFrame = App::GetTimer().GetTotalFrameCount();

    // Can't append while iterating
    SmallVector<uint64, App::FrameAllocator, 3> toAppend;

    for (auto it = m_instanceUpdates.begin_it(); it != m_instanceUpdates.end_it(); 
        it = m_instanceUpdates.next_it(it))
    {
        const auto instance = it->Key;
        const auto frame = it->Val;
        const TreePos& p = FindTreePosFromID(instance).value();

        // -1 -> update was added at the tail end of last frame
        if (frame < currFrame - 1)
        {
            // Mesh hasn't moved, just update previous transformation
            m_prevToWorlds[instance] = m_sceneGraph[p.Level].m_toWorlds[p.Offset];
            continue;
        }

        // Grab current to world transformation
        const float4x3& prevW = m_sceneGraph[p.Level].m_toWorlds[p.Offset];
        v_float4x4 vW = load4x3(prevW);

        float4a t;
        float4a r;
        float4a s;
        decomposeSRT(vW, s, r, t);

        Assert(r.w <= 1 && r.w >= -1, "");

        Assert(!isnan(r.x) && !isnan(r.y) && !isnan(r.z) && !isnan(r.w), "");

        // Apply the update
        auto& delta = m_tempWorldTransformUpdates[instance];
        float3 newTr = delta.Tr + t.xyz();
        float3 newScale = delta.Scale * s.xyz();

        v_float4x4 vR = rotationMatFromQuat(load(r));
        v_float4x4 vNewR = load3x3(delta.Rotation);
        vR = mul(vR, vNewR);

        v_float4x4 vNewWorld = affineTransformation(vR, newScale, newTr);

        float3x3 R = float3x3(store(vNewR));
        Assert(fabsf(R.m[0].length() - 1) < 1e-5, "");
        Assert(fabsf(R.m[1].length() - 1) < 1e-5, "");
        Assert(fabsf(R.m[2].length() - 1) < 1e-5, "");

        // Update previous & current transformations
        m_prevToWorlds[instance] = prevW;
        m_sceneGraph[p.Level].m_toWorlds[p.Offset] = float4x3(store(vNewWorld));
        
        // Add subtree to stack
        if (const auto range = m_sceneGraph[p.Level].m_subtreeRanges[p.Offset]; range.Count)
        {
            stack.push_back(Entry{ .W = vNewWorld,
                .TreeLevel = p.Level, 
                .Base = range.Base, 
                .Count = range.Count });
        }

        // Remember transformation update for future
        if (auto existingIt = m_worldTransformUpdates.find(instance); existingIt)
        {
            auto& curr = *existingIt.value();
            curr.Translation += delta.Tr;
            curr.Scale *= delta.Scale;

            v_float4x4 vCurrR = rotationMatFromQuat(loadFloat4(curr.Rotation));
            vCurrR = mul(vCurrR, vNewR);
            curr.Rotation = quaternionFromRotationMat1(vCurrR);

            Assert(!isnan(curr.Rotation.x) && !isnan(curr.Rotation.y) && !isnan(curr.Rotation.z) && !isnan(curr.Rotation.w), "");

        }
        else
        {
            AffineTransformation tr;
            tr.Translation = delta.Tr;
            tr.Scale = delta.Scale;
            tr.Rotation = quaternionFromRotationMat1(vNewR);

            m_worldTransformUpdates[instance] = tr;
        }
    }

    while (!stack.empty())
    {
        Entry e = stack.back();
        stack.pop_back();
        auto& currLevel = m_sceneGraph[e.TreeLevel + 1];

        for (size_t j = e.Base; j < e.Base + e.Count; j++)
        {
            Assert(RT_Flags::Decode(currLevel.m_rtFlags[j]).MeshMode ==
                RT_MESH_MODE::DYNAMIC_NO_REBUILD, "Invalid scene graph.");

            const uint64_t ID = m_sceneGraph[e.TreeLevel + 1].m_IDs[j];
            toAppend.push_back(ID);

            AffineTransformation& local = currLevel.m_localTransforms[j];
            v_float4x4 vLocal = affineTransformation(local.Scale, local.Rotation, local.Translation);
            v_float4x4 vNewWorld = mul(vLocal, e.W);

            // If instance has had updates, apply them
            if (auto updateIt = m_worldTransformUpdates.find(ID); updateIt)
            {
                float4a t;
                float4a s;
                v_float4x4 vR = decomposeSRT(vNewWorld, s, t);

                AffineTransformation& existing = *updateIt.value();
                float3 newTr = existing.Translation + t.xyz();
                float3 newScale = existing.Scale * s.xyz();

                v_float4x4 vRotUpdate = rotationMatFromQuat(loadFloat4(existing.Rotation));
                vR = mul(vR, vRotUpdate);

                vNewWorld = affineTransformation(vR, newScale, newTr);
            }

            // Update previous & current transformations
            m_prevToWorlds[ID] = currLevel.m_toWorlds[j];
            currLevel.m_toWorlds[j] = float4x3(store(vNewWorld));

            // Add subtree to stack
            if (const auto& subtree = currLevel.m_subtreeRanges[j]; subtree.Count)
            {
                stack.push_back(Entry{ .W = vNewWorld,
                    .TreeLevel = e.TreeLevel + 2,
                    .Base = subtree.Base,
                    .Count = subtree.Count });
            }
        }
    }

    m_tempWorldTransformUpdates.clear();
    
    for(auto id : toAppend)
        m_instanceUpdates[id] = App::GetTimer().GetTotalFrameCount() - 1;
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
            const __m128 vTranslate1 = loadFloat3(k1.Transform.Translation);
            const __m128 vTranslate2 = loadFloat3(k2.Transform.Translation);
            const __m128 vTranslateInt = lerp(vTranslate1, vTranslate2, interpolatedT);

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

bool SceneCore::ConvertInstanceDynamic(uint64_t instanceID)
{
    const TreePos& p = FindTreePosFromID(instanceID).value();
    const auto currFlags = RT_Flags::Decode(m_sceneGraph[p.Level].m_rtFlags[p.Offset]);

    if (currFlags.MeshMode == RT_MESH_MODE::STATIC)
    {
        m_sceneGraph[p.Level].m_rtFlags[p.Offset] = RT_Flags::Encode(
            RT_MESH_MODE::DYNAMIC_NO_REBUILD,
            currFlags.InstanceMask, 1, 0, currFlags.IsOpaque);

        m_pendingRtMeshModeSwitch.push_back(instanceID);
        m_numStaticInstances--;
        m_numDynamicInstances++;

        auto subtree = m_sceneGraph[p.Level].m_subtreeRanges[p.Offset];
        if (subtree.Count)
            ConvertSubTreeDynamic(p.Level + 1, subtree);

        return true;
    }

    return false;
}

void SceneCore::ConvertSubTreeDynamic(uint32_t treeLevel, Range r)
{
    for (size_t i = r.Base; i < r.Base + r.Count; i++)
    {
        auto rtFlags = RT_Flags::Decode(m_sceneGraph[treeLevel].m_rtFlags[i]);
        if (rtFlags.MeshMode != Model::RT_MESH_MODE::DYNAMIC_NO_REBUILD)
        {
            m_sceneGraph[treeLevel].m_rtFlags[i] = RT_Flags::Encode(
                Model::RT_MESH_MODE::DYNAMIC_NO_REBUILD,
                rtFlags.InstanceMask, 1, 0, rtFlags.IsOpaque);

            m_pendingRtMeshModeSwitch.push_back(m_sceneGraph[treeLevel].m_IDs[i]);
            m_numStaticInstances--;
            m_numDynamicInstances++;
        }

        if (m_sceneGraph[treeLevel].m_subtreeRanges[i].Count > 0)
            ConvertSubTreeDynamic(treeLevel + 1, m_sceneGraph[treeLevel].m_subtreeRanges[i]);
    }
}

void SceneCore::AnimateCallback(const ParamVariant& p)
{
    m_animate = !p.GetBool();
}