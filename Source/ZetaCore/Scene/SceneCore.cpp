#include "SceneCore.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Quaternion.h"
#include "../Support/Task.h"
#include "Camera.h"
#include <App/Timer.h>
#include <Support/Param.h>
#include <algorithm>
#include "../Assets/Font/IconsFontAwesome6.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
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
    ZetaInline uint3 Pcg3d(uint3 v)
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
    m_normalDescTable.Init(XXH3_64bits(GlobalResource::NORMAL_DESCRIPTOR_TABLE, 
        strlen(GlobalResource::NORMAL_DESCRIPTOR_TABLE)));
    m_metallicRoughnessDescTable.Init(XXH3_64bits(GlobalResource::METALLIC_ROUGHNESS_DESCRIPTOR_TABLE,
        strlen(GlobalResource::METALLIC_ROUGHNESS_DESCRIPTOR_TABLE)));
    m_emissiveDescTable.Init(XXH3_64bits(GlobalResource::EMISSIVE_DESCRIPTOR_TABLE, 
        strlen(GlobalResource::EMISSIVE_DESCRIPTOR_TABLE)));

    m_rendererInterface.Init();

    // Allocate a slot for the default material
    m_matBuffer.ResizeAdditionalMaterials(1);

    Material defaultMat;
    m_matBuffer.Add(DEFAULT_MATERIAL_ID, defaultMat);

    ParamVariant animation;
    animation.InitBool(ICON_FA_LANDMARK " Scene", "Animation", "Pause",
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

            m_rebuildBVHFlag = false;
        });

    const uint32_t numInstances = m_emissives.NumInstances();
    m_staleEmissiveMats = m_emissives.HasStaleMaterials() || !m_emissives.Initialized();
    // Size of m_instanceUpdates may change after async. task above runs, but since it never
    // goes from > 0 to 0, it doesn't matter
    m_staleEmissivePositions = m_staleEmissivePositions || !m_emissives.Initialized();

    if (!m_emissives.Initialized() && numInstances)
    {
        ParamVariant emissives;
        emissives.InitBool(ICON_FA_LANDMARK " Scene", "Emissives", "Enabled",
            fastdelegate::MakeDelegate(this, &SceneCore::ToggleEmissivesCallback),
            !m_ignoreEmissives);
        App::AddParam(emissives);
    }

    // When emissives have stale position or material
    if (numInstances && (m_staleEmissivePositions || m_staleEmissiveMats))
    {
        TaskSet::TaskHandle resetRtAsInfo = TaskSet::INVALID_TASK_HANDLE;

        // NOTE RT-AS info is needed to compute a unique hash for emissives. It is managed
        // by TLAS, but since that runs later, it's not available for initialization
        // of emissives. For future frames, TLAS expects the old (stale) RT-AS info 
        // so it can't be changed here. 
        // 
        // TODO In the case of StaticToDynamic, the first UpdateEmissivePositions() call 
        // uses the wrong InstanceID. Later in the frame, it's updated by TLAS and from 
        // the second frame on, the correct value is used. Since movement usually lasts 
        // for more than one frame, it shouldn't be a problem.
#if 0
        if (m_staleEmissivePositions)
#else
        if (!m_emissives.Initialized())
#endif
        {
            resetRtAsInfo = sceneTS.EmplaceTask("Scene::UpdateRtAsInfo", [this]()
                {
                    ResetRtAsInfos();
                });
        }

        auto upload = sceneTS.EmplaceTask("UploadEmissiveBuffer", [this]()
            {
                m_emissives.UploadToGPU();
            });

        // Full rebuild of emissive buffer for first time
        if (!m_emissives.Initialized())
        {
            constexpr size_t MAX_NUM_EMISSIVE_WORKERS = 5;
            constexpr size_t MIN_EMISSIVE_INSTANCES_PER_WORKER = 35;
            size_t threadOffsets[MAX_NUM_EMISSIVE_WORKERS];
            size_t threadSizes[MAX_NUM_EMISSIVE_WORKERS];

            const size_t numEmissiveWorkers = SubdivideRangeWithMin(numInstances,
                MAX_NUM_EMISSIVE_WORKERS,
                threadOffsets,
                threadSizes,
                MIN_EMISSIVE_INSTANCES_PER_WORKER);

            for (size_t i = 0; i < numEmissiveWorkers; i++)
            {
                StackStr(tname, n, "Scene::Emissive_%d", i);

                auto h = sceneTS.EmplaceTask(tname, [this, offset = threadOffsets[i], size = threadSizes[i]]()
                    {
                        auto emissvies = m_emissives.Instances();
                        auto tris = m_emissives.Triagnles();
                        auto triInitialPos = m_emissives.InitialTriPositions();
                        v_float4x4 I = identity();

                        // For every emissive instance, apply world transformation to all of its triangles
                        for (size_t instance = offset; instance < offset + size; instance++)
                        {
                            const auto& e = emissvies[instance];
                            const v_float4x4 vW = load4x3(GetToWorld(e.InstanceID));
                            const bool skipTransform = equal(vW, I);

                            const auto rtASInfo = GetInstanceRtASInfo(e.InstanceID);

                            for (size_t t = e.BaseTriOffset; t < e.BaseTriOffset + e.NumTriangles; t++)
                            {
                                if (!skipTransform)
                                {
                                    __m128 vV0;
                                    __m128 vV1;
                                    __m128 vV2;
                                    tris[t].LoadVertices(vV0, vV1, vV2);

                                    triInitialPos[t].Vtx0 = tris[t].Vtx0;
                                    triInitialPos[t].V0V1 = tris[t].V0V1;
                                    triInitialPos[t].V0V2 = tris[t].V0V2;
                                    triInitialPos[t].EdgeLengths = tris[t].EdgeLengths;
                                    triInitialPos[t].PrimIdx = tris[t].ID;

                                    vV0 = mul(vW, vV0);
                                    vV1 = mul(vW, vV1);
                                    vV2 = mul(vW, vV2);
                                    tris[t].StoreVertices(vV0, vV1, vV2);
                                }

                                const uint32_t hash = Pcg3d(uint3(rtASInfo.GeometryIndex, 
                                    rtASInfo.InstanceID,
                                    tris[t].ID)).x;

                                Assert(!tris[t].IsIDPatched(), 
                                    "Rewriting emissive triangle ID after the first assignment is invalid.");
                                tris[t].ResetID(hash);
                            }
                        }
                    });

                sceneTS.AddOutgoingEdge(updateWorldTransforms, h);

                Assert(resetRtAsInfo != TaskSet::INVALID_TASK_HANDLE, "Invalid task handle.");
                sceneTS.AddOutgoingEdge(resetRtAsInfo, h);

                sceneTS.AddOutgoingEdge(h, upload);
            }
        }
        else if (m_staleEmissivePositions)
        {
            auto h = sceneTS.EmplaceTask("Scene::UpdateEmissivePos", [this]()
                {
                    UpdateEmissivePositions();
                });

            //sceneTS.AddOutgoingEdge(resetRtAsInfo, h);
            sceneTS.AddOutgoingEdge(h, upload);
        }

        m_staleEmissivePositions = false;
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

    for (auto& heap : m_textureHeaps)
        heap.Reset();

    m_rendererInterface.Shutdown();
}

uint32_t SceneCore::AddMesh(SmallVector<Vertex>&& vertices, SmallVector<uint32_t>&& indices,
    uint32_t matIdx, bool lock)
{
    if (lock)
        AcquireSRWLockExclusive(&m_meshLock);

    m_numTriangles += (uint32_t)indices.size();
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

    m_numTriangles += (uint32_t)indices.size();
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

void SceneCore::AddMaterial(const Asset::MaterialDesc& matDesc, MutableSpan<Texture> ddsImages,
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

    auto addTex = [](Texture::ID_TYPE ID, const char* type, TexSRVDescriptorTable& table, uint32_t& tableOffset, 
        MutableSpan<Texture> ddsImages)
        {
            auto idx = BinarySearch(Span(ddsImages), ID, [](const Texture& obj) {return obj.ID(); });
            Check(idx != -1, "%s image with ID %llu was not found.", type, ID);

            tableOffset = table.Add(ZetaMove(ddsImages[idx]));

            // HACK Since the texture was moved, ID was changed to -1. Add a dummy texture with the same ID
            // so that binary search continues to work.
            ddsImages[idx] = Texture(ID, nullptr, RESOURCE_HEAP_TYPE::COMMITTED);
        };

    if (lock)
        AcquireSRWLockExclusive(&m_matLock);

    {
        uint32_t tableOffset = Material::INVALID_ID;    // i.e. index in GPU descriptor table

        if (matDesc.BaseColorTexID != Texture::INVALID_ID)
        {
            addTex(matDesc.BaseColorTexID, "BaseColor", m_baseColorDescTable, tableOffset, ddsImages);
            mat.SetBaseColorTex(tableOffset);
        }
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.NormalTexID != Texture::INVALID_ID)
        {
            addTex(matDesc.NormalTexID, "NormalMap", m_normalDescTable, tableOffset, ddsImages);
            mat.SetNormalTex(tableOffset);
        }
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.MetallicRoughnessTexID != Texture::INVALID_ID)
        {
            addTex(matDesc.MetallicRoughnessTexID, "MetallicRoughnessMap",
                m_metallicRoughnessDescTable, tableOffset, ddsImages);

            mat.SetMetallicRoughnessTex(tableOffset);
        }
    }

    {
        uint32_t tableOffset = Material::INVALID_ID;
        if (matDesc.EmissiveTexID != Texture::INVALID_ID)
        {
            addTex(matDesc.EmissiveTexID, "EmissiveMap", m_emissiveDescTable, tableOffset, ddsImages);
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

    m_rebuildBVHFlag = true;

    if (lock)
        ReleaseSRWLockExclusive(&m_instanceLock);
}

uint32_t SceneCore::InsertAtLevel(uint64_t id, uint32_t treeLevel, uint32_t parentIdx, 
    AffineTransformation& localTransform, uint64_t meshID, RT_MESH_MODE rtMeshMode, 
    uint8_t rtInstanceMask, bool isOpaque)
{
    Assert(m_sceneGraph.size() > treeLevel, "Scene graph hasn't been preallocated.");
    //m_sceneGraph.resize(Max(treeLevel + 1, (uint32_t)m_sceneGraph.size()));

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
        if (numToMove)
            memmove(vec.data() + insertIdx + 1, vec.data() + insertIdx, numToMove * sizeof(T));

        // Construct the new entry in-place
        new (vec.data() + insertIdx) T(ZetaForward(args)...);
    };

    float4x3 I = float4x3(store(identity()));

    Assert(insertIdx <= currLevel.m_IDs.size(), "Out-of-bounds insertion index.");
    Assert(currLevel.m_IDs.capacity() >= currLevel.m_IDs.size() + 1, "Scene graph hasn't been preallocated.");
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

void SceneCore::ResetRtAsInfos()
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

    if (m_numDynamicInstances == 0)
        return;

    currInstance = 0;

    for (size_t treeLevelIdx = 1; treeLevelIdx < m_sceneGraph.size(); treeLevelIdx++)
    {
        auto& currTreeLevel = m_sceneGraph[treeLevelIdx];
        auto& rtFlagVec = currTreeLevel.m_rtFlags;

        for (size_t i = 0; i < rtFlagVec.size(); i++)
        {
            const Scene::RT_Flags flags = RT_Flags::Decode(currTreeLevel.m_rtFlags[i]);

            if (flags.MeshMode != RT_MESH_MODE::STATIC)
            {
                const uint64_t meshID = currTreeLevel.m_meshIDs[i];
                if (meshID == Scene::INVALID_MESH)
                    continue;

                currTreeLevel.m_rtASInfo[i] = RT_AS_Info{
                    .GeometryIndex = 0,
                    .InstanceID = m_numStaticInstances + currInstance };

                currInstance++;
            }
        }
    }
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

    const auto treePos = FindTreePosFromID(id).value();
    const auto rtFlags = RT_Flags::Decode(m_sceneGraph[treePos.Level].m_rtFlags[treePos.Offset]);

    m_staleEmissivePositions = m_staleEmissivePositions || 
        (m_emissives.NumInstances() &&
        (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE));

    ConvertInstanceDynamic(id, treePos, rtFlags);
    // Updates if instance already exists
    m_instanceUpdates[id] = App::GetTimer().GetTotalFrameCount();

    m_rendererInterface.SceneModified();
}

void SceneCore::ReserveInstances(Span<int> treeLevels, size_t total)
{
    Assert(treeLevels.size() > 0, "Invalid tree.");

    // +1 for root
    m_sceneGraph.resize(treeLevels.size() + 1);
    for (size_t i = 0; i < treeLevels.size(); i++)
    {
        m_sceneGraph[i + 1].m_IDs.reserve(treeLevels[i]);
        m_sceneGraph[i + 1].m_localTransforms.reserve(treeLevels[i]);
        m_sceneGraph[i + 1].m_meshIDs.reserve(treeLevels[i]);
        m_sceneGraph[i + 1].m_rtASInfo.reserve(treeLevels[i]);
        m_sceneGraph[i + 1].m_rtFlags.reserve(treeLevels[i]);
        m_sceneGraph[i + 1].m_subtreeRanges.reserve(treeLevels[i]);
        m_sceneGraph[i + 1].m_toWorlds.reserve(treeLevels[i]);
    }

    m_prevToWorlds.resize(total, true);
    m_IDtoTreePos.resize(total, true);
    m_worldTransformUpdates.resize(Min(total, 32llu));
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

void SceneCore::UpdateEmissiveMaterial(uint64_t instanceID, const float3& emissiveFactor, float strength)
{
    m_emissives.UpdateMaterial(instanceID, emissiveFactor, strength);
    m_rendererInterface.SceneModified();
}

void SceneCore::ToggleEmissivesCallback(const Support::ParamVariant& p)
{
    m_ignoreEmissives = !p.GetBool();
    m_rendererInterface.SceneModified();
    m_rendererInterface.ToggleEmissives();
}

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

void SceneCore::UpdateEmissivePositions()
{
    auto tris = m_emissives.Triagnles();
    auto triInitialPos = m_emissives.InitialTriPositions();

    uint32_t minIdx = (uint32_t)tris.size() - 1;
    uint32_t maxIdx = 0;

    for (auto it = m_instanceUpdates.begin_it(); it != m_instanceUpdates.end_it();
        it = m_instanceUpdates.next_it(it))
    {
        auto instance = it->Key;
        const auto& emissiveInstance = *m_emissives.FindInstance(instance).value();
        const v_float4x4 vW = load4x3(GetToWorld(instance));
        const auto rtASInfo = GetInstanceRtASInfo(instance);

        for (size_t t = emissiveInstance.BaseTriOffset; 
            t < emissiveInstance.BaseTriOffset + emissiveInstance.NumTriangles; t++)
        {
            EmissiveBuffer::Triangle& initTri = triInitialPos[t];

            __m128 vV0;
            __m128 vV1;
            __m128 vV2;
            RT::EmissiveTriangle::DecodeVertices(initTri.Vtx0, initTri.V0V1, initTri.V0V2,
                initTri.EdgeLengths,
                vV0, vV1, vV2);

            vV0 = mul(vW, vV0);
            vV1 = mul(vW, vV1);
            vV2 = mul(vW, vV2);
            tris[t].StoreVertices(vV0, vV1, vV2);

            // Dynamic instances have geometry index = 0
            const uint32_t hash = Pcg3d(uint3(0,
                rtASInfo.InstanceID,
                initTri.PrimIdx)).x;
            tris[t].ID = hash;
        }

        minIdx = Min(minIdx, emissiveInstance.BaseTriOffset);
        maxIdx = Max(maxIdx, emissiveInstance.BaseTriOffset + emissiveInstance.NumTriangles);
    }

    Assert(minIdx <= maxIdx, "Invalid indices.");
    m_emissives.UpdateTriPositions(minIdx, maxIdx);
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

bool SceneCore::ConvertInstanceDynamic(uint64_t instanceID, const TreePos& treePos, 
    RT_Flags rtFlags)
{
    if (rtFlags.MeshMode == RT_MESH_MODE::STATIC)
    {
        m_sceneGraph[treePos.Level].m_rtFlags[treePos.Offset] = RT_Flags::Encode(
            RT_MESH_MODE::DYNAMIC_NO_REBUILD,
            rtFlags.InstanceMask, 1, 0, rtFlags.IsOpaque);

        m_pendingRtMeshModeSwitch.push_back(instanceID);
        m_numStaticInstances--;
        m_numDynamicInstances++;

        auto subtree = m_sceneGraph[treePos.Level].m_subtreeRanges[treePos.Offset];
        if (subtree.Count)
            ConvertSubtreeDynamic(treePos.Level + 1, subtree);

        return true;
    }

    return false;
}

void SceneCore::ConvertSubtreeDynamic(uint32_t treeLevel, Range r)
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
            ConvertSubtreeDynamic(treeLevel + 1, m_sceneGraph[treeLevel].m_subtreeRanges[i]);
    }
}

void SceneCore::AnimateCallback(const ParamVariant& p)
{
    m_animate = !p.GetBool();
}

void SceneCore::ClearPick()
{
    m_rendererInterface.ClearPick();

    AcquireSRWLockExclusive(&m_pickLock);
    m_pickedInstances.clear();
    ReleaseSRWLockExclusive(&m_pickLock);
}

void SceneCore::SetPickedInstance(uint64 instanceID)
{
    AcquireSRWLockExclusive(&m_pickLock);

    if (!m_multiPick)
    {
        m_pickedInstances.resize(1);
        m_pickedInstances[0] = instanceID;
    }
    else
    {
        // NOTE usually there aren't more than a few objects picked
        // at the same time, so linear search should be fine
        bool found = false;
        for (size_t i = 0; i < m_pickedInstances.size(); i++)
        {
            if (m_pickedInstances[i] == instanceID)
            {
                m_pickedInstances.erase_at_index(i);
                found = true;
                break;
            }
        }

        if(!found)
            m_pickedInstances.push_back(instanceID);
    }

    ReleaseSRWLockExclusive(&m_pickLock);
}
