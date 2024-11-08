#include "glTF.h"
#include "../Math/MatrixFuncs.h"
#include "../Math/Surface.h"
#include "../Math/Quaternion.h"
#include "../Scene/SceneCore.h"
#include "../Support/Task.h"
#include "../App/Log.h"
#include "../Utility/Utility.h"
#include <algorithm>

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>

using namespace ZetaRay;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Model;
using namespace ZetaRay::App;
using namespace ZetaRay::Model::glTF::Asset;
using namespace ZetaRay::Core::Direct3DUtil;

#define CHECK_QUATERNION_VALID 0

//--------------------------------------------------------------------------------------
// glTF
//--------------------------------------------------------------------------------------

namespace
{
    ZetaInline const char* GetErrorMsg(cgltf_result r)
    {
        switch (r)
        {
        case cgltf_result_data_too_short:
            return "cgltf_result_data_too_short";
        case cgltf_result_unknown_format:
            return "cgltf_result_unknown_format";
        case cgltf_result_invalid_json:
            return "cgltf_result_invalid_json";
        case cgltf_result_invalid_gltf:
            return "cgltf_result_invalid_gltf";
        case cgltf_result_invalid_options:
            return "cgltf_result_invalid_options";
        case cgltf_result_file_not_found:
            return "cgltf_result_file_not_found";
        case cgltf_result_io_error:
            return "cgltf_result_io_error";
        case cgltf_result_out_of_memory:
            return "cgltf_result_out_of_memory";
        case cgltf_result_legacy_gltf:
            return "cgltf_result_legacy_gltf";
        default:
            return "unknown error";
        }
    }

#ifndef Checkgltf
#define Checkgltf(expr)                                                                                                           \
    {                                                                                                                             \
        cgltf_result r = (expr);                                                                                                  \
        if (r != cgltf_result_success)                                                                                            \
        {                                                                                                                         \
            char buff_[256];                                                                                                      \
            int n_ = stbsp_snprintf(buff_, 256, "cgltf call failed at %s: %d\nError: %s", __FILE__, __LINE__, GetErrorMsg(r));    \
            ZetaRay::Util::ReportError("Fatal Error", buff_);                                                                     \
            ZetaRay::Util::DebugBreak();                                                                                          \
        }                                                                                                                         \
    }
#endif

    // Remember every emissive mesh primitive
    struct EmissiveMeshPrim
    {
        uint64_t MeshID;
        uint32_t BaseVtxOffset;
        uint32_t BaseIdxOffset;
        uint32_t NumIndices;
        int MaterialIdx;
    };

    struct ThreadContext
    {
        uint32_t SceneID;
        const App::Filesystem::Path& Path;
        cgltf_data* Model;
        int NumMeshWorkers;
        int NumImgWorkers;
        size_t* MeshThreadOffsets;
        size_t* MeshThreadSizes;
        size_t* ImgThreadOffsets;
        size_t* ImgThreadSizes;
        SmallVector<Vertex> Vertices;
        std::atomic_uint32_t& CurrVtxOffset;
        SmallVector<uint32_t> Indices;
        std::atomic_uint32_t& CurrIdxOffset;
        SmallVector<Mesh> Meshes;
        std::atomic_uint32_t& CurrMeshPrimOffset;
        MutableSpan<EmissiveMeshPrim> EmissiveMeshPrims;
        uint32_t* EmissiveMeshPrimCountPerWorker;
        SmallVector<RT::EmissiveTriangle>& RTEmissives;
        SmallVector<EmissiveInstance>& EmissiveInstances;
        int NumEmissiveMeshPrims;
        int NumEmissiveInstances;
        uint32_t NumEmissiveTris;
    };

    void ResetEmissiveSubsets(MutableSpan<EmissiveMeshPrim> subsets)
    {
        if (subsets.empty())
            return;

        const int numTotalBytes = (int)subsets.size() * sizeof(EmissiveMeshPrim);
        const int numSimdBytes = numTotalBytes >> 5;
        const int numRemainingBytes = numTotalBytes & 31;
        int numToSetManually = numRemainingBytes > 0 ? Math::CeilUnsignedIntDiv(numRemainingBytes, 
            (int)sizeof(EmissiveMeshPrim)) : 0;

        uintptr_t ptr = reinterpret_cast<uintptr_t>(subsets.data());
        __m256i vVal = _mm256_set1_epi64x(Scene::INVALID_MESH);

        for (int i = 0; i < numSimdBytes; i++)
        {
            _mm256_storeu_si256((__m256i*) ptr, vVal);
            ptr += 32;
        }

        for (int i = 0; i < numToSetManually; i++)
            subsets[subsets.size() - 1 - i].MeshID = Scene::INVALID_MESH;
    }

    void ProcessPositions(const cgltf_data& model, const cgltf_accessor& accessor, 
        MutableSpan<Vertex> vertices, uint32_t baseOffset)
    {
        Check(accessor.type == cgltf_type_vec3, "Invalid type for POSITION attribute.");
        Check(accessor.component_type == cgltf_component_type_r_32f,
            "Invalid component type for POSITION attribute.");

        const cgltf_buffer_view& bufferView = *accessor.buffer_view;
        Check(accessor.stride == sizeof(float3), "Invalid stride for POSITION attribute.");

        const cgltf_buffer& buffer = *bufferView.buffer;
        const float3* start = reinterpret_cast<float3*>(reinterpret_cast<uintptr_t>(
            buffer.data) + bufferView.offset + accessor.offset);

        for (size_t i = 0; i < accessor.count; i++)
        {
            const float3* curr = start + i;

            // glTF uses a right-handed coordinate system with +Y as up
            vertices[baseOffset + i].Position = float3(curr->x, curr->y, -curr->z);
        }
    }

    void ProcessNormals(const cgltf_data& model, const cgltf_accessor& accessor, 
        MutableSpan<Vertex> vertices, uint32_t baseOffset)
    {
        Check(accessor.type == cgltf_type_vec3, "Invalid type for NORMAL attribute.");
        Check(accessor.component_type == cgltf_component_type_r_32f,
            "Invalid component type for NORMAL attribute.");

        const cgltf_buffer_view& bufferView = *accessor.buffer_view;
        Check(accessor.stride == sizeof(float3), "Invalid stride for NORMAL attribute.");

        const cgltf_buffer& buffer = *bufferView.buffer;
        const float3* start = reinterpret_cast<float3*>(reinterpret_cast<uintptr_t>(
            buffer.data) + bufferView.offset + accessor.offset);

        for (size_t i = 0; i < accessor.count; i++)
        {
            const float3* curr = start + i;

            // glTF uses a right-handed coordinate system with +Y as up
            vertices[baseOffset + i].Normal = oct32(curr->x, curr->y, -curr->z);
        }
    }

    void ProcessTexCoords(const cgltf_data& model, const cgltf_accessor& accessor, 
        MutableSpan<Vertex> vertices, uint32_t baseOffset)
    {
        Check(accessor.type == cgltf_type_vec2, "Invalid type for TEXCOORD_0 attribute.");
        Check(accessor.component_type == cgltf_component_type_r_32f,
            "Invalid component type for TEXCOORD_0 attribute.");

        const cgltf_buffer_view& bufferView = *accessor.buffer_view;
        Check(accessor.stride == sizeof(float2), "Invalid stride for TEXCOORD_0 attribute.");

        const cgltf_buffer& buffer = *bufferView.buffer;
        const float2* start = reinterpret_cast<float2*>(reinterpret_cast<uintptr_t>(
            buffer.data) + bufferView.offset + accessor.offset);

        for (size_t i = 0; i < accessor.count; i++)
        {
            const float2* curr = start + i;
            vertices[baseOffset + i].TexUV = float2(curr->x, curr->y);
        }
    }

    void ProcessTangents(const cgltf_data& model, const cgltf_accessor& accessor, 
        MutableSpan<Vertex> vertices, uint32_t baseOffset)
    {
        Check(accessor.type == cgltf_type_vec4, "Invalid type for TANGENT attribute.");
        Check(accessor.component_type == cgltf_component_type_r_32f,
            "Invalid component type for TANGENT attribute.");

        const cgltf_buffer_view& bufferView = *accessor.buffer_view;

        const cgltf_buffer& buffer = *bufferView.buffer;
        const float4* start = reinterpret_cast<float4*>(reinterpret_cast<uintptr_t>(
            buffer.data) + bufferView.offset + accessor.offset);

        for (size_t i = 0; i < accessor.count; i++)
        {
            const float4* curr = start + i;

            // glTF uses a right-handed coordinate system with +Y as up
            vertices[baseOffset + i].Tangent = oct32(curr->x, curr->y, -curr->z);
        }
    }

    void ProcessIndices(const cgltf_data& model, const cgltf_accessor& accessor, 
        MutableSpan<uint32_t> indices, uint32_t baseOffset)
    {
        Check(accessor.type == cgltf_type_scalar, "Invalid index type.");
        Check(accessor.stride != -1, "Invalid index stride.");
        Check(accessor.count % 3 == 0, "Invalid number of indices.");

        const cgltf_buffer_view& bufferView = *accessor.buffer_view;
        const cgltf_buffer& buffer = *bufferView.buffer;

        // Populate the mesh indices
        uint8_t* curr = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buffer.data) + bufferView.offset + accessor.offset);
        const size_t numFaces = accessor.count / 3;
        const size_t indexStrideInBytes = accessor.stride;
        size_t currIdxOffset = 0;

        for (size_t face = 0; face < numFaces; face++)
        {
            uint32_t i0 = 0;
            uint32_t i1 = 0;
            uint32_t i2 = 0;

            memcpy(&i0, curr, indexStrideInBytes);
            curr += indexStrideInBytes;
            memcpy(&i1, curr, indexStrideInBytes);
            curr += indexStrideInBytes;
            memcpy(&i2, curr, indexStrideInBytes);
            curr += indexStrideInBytes;

            // Use clockwise ordering
            indices[baseOffset + currIdxOffset++] = i0;
            indices[baseOffset + currIdxOffset++] = i2;
            indices[baseOffset + currIdxOffset++] = i1;
        }
    }

    void ProcessMeshes(const cgltf_data& model, uint32_t sceneID, size_t offset, size_t size,
        MutableSpan<Vertex> vertices, std::atomic_uint32_t& vertexCounter,
        MutableSpan<uint32_t> indices, std::atomic_uint32_t& idxCounter,
        MutableSpan<Mesh> meshes, std::atomic_uint32_t& meshCounter,
        MutableSpan<EmissiveMeshPrim> emissivesPrims, uint32_t& emissivePrimCount)
    {
        SceneCore& scene = App::GetScene();
        uint32_t totalPrims = 0;
        uint32_t totalVertices = 0;
        uint32_t totalIndices = 0;
        int numEmissiveMeshPrims = 0;

        // Count total number of primitives, vertices, and indices.
        for (size_t meshIdx = offset; meshIdx != offset + size; meshIdx++)
        {
            Assert(meshIdx < model.meshes_count, "Out-of-bound access");
            const cgltf_mesh& mesh = model.meshes[meshIdx];

            for (int primIdx = 0; primIdx < mesh.primitives_count; primIdx++)
            {
                const cgltf_primitive& prim = mesh.primitives[primIdx];

                Check(prim.indices->count > 0, "Index buffer is required.");
                Check(prim.type == cgltf_primitive_type_triangles, "Non-triangle meshes are not supported.");

                int posIt = -1;

                for (int attrib = 0; attrib < prim.attributes_count; attrib++)
                {
                    if (strcmp(prim.attributes[attrib].name, "POSITION") == 0)
                    {
                        posIt = attrib;
                        break;
                    }
                }

                Check(posIt != -1, "POSITION was not found in the vertex attributes.");

                const cgltf_accessor& accessor = *prim.attributes[posIt].data;
                const uint32_t numVertices = (uint32_t)accessor.count;
                totalVertices += numVertices;

                const uint32_t numIndices = (uint32_t)prim.indices->count;
                totalIndices += numIndices;
            }

            totalPrims += (uint32_t)mesh.primitives_count;
        }

        // (sub)allocate
        const uint32_t workerBaseVtxOffset = vertexCounter.fetch_add(totalVertices, std::memory_order_relaxed);
        const uint32_t workerBaseIdxOffset = idxCounter.fetch_add(totalIndices, std::memory_order_relaxed);
        const uint32_t workerPrimBaseOffset = meshCounter.fetch_add(totalPrims, std::memory_order_relaxed);
        const uint32_t workerBaseEmissiveOffset = workerPrimBaseOffset;

        uint32_t currVtxOffset = workerBaseVtxOffset;
        uint32_t currIdxOffset = workerBaseIdxOffset;
        uint32_t currMeshPrimOffset = workerPrimBaseOffset;

        // Now iterate again and populate the buffers
        for (size_t meshIdx = offset; meshIdx != offset + size; meshIdx++)
        {
            const cgltf_mesh& mesh = model.meshes[meshIdx];

            for (int primIdx = 0; primIdx < mesh.primitives_count; primIdx++)
            {
                const cgltf_primitive& prim = mesh.primitives[primIdx];

                int posIt = -1;
                int normalIt = -1;
                int texIt = -1;
                int tangentIt = -1;

                for (int attrib = 0; attrib < prim.attributes_count; attrib++)
                {
                    if (strcmp(prim.attributes[attrib].name, "POSITION") == 0)
                        posIt = attrib;
                    else if (strcmp(prim.attributes[attrib].name, "NORMAL") == 0)
                        normalIt = attrib;
                    else if (strcmp(prim.attributes[attrib].name, "TEXCOORD_0") == 0)
                        texIt = attrib;
                    else if (strcmp(prim.attributes[attrib].name, "TANGENT") == 0)
                        tangentIt = attrib;
                }

                Check(normalIt != -1, "NORMAL was not found in the vertex attributes.");

                // Populate the vertex attributes
                const cgltf_accessor& accessor = *prim.attributes[posIt].data;
                const uint32_t numVertices = (uint32_t)accessor.count;

                const cgltf_buffer_view& bufferView = *prim.indices->buffer_view;
                const uint32_t numIndices = (uint32_t)prim.indices->count;

                // POSITION
                ProcessPositions(model, *prim.attributes[posIt].data, vertices, currVtxOffset);

                // NORMAL
                ProcessNormals(model, *prim.attributes[normalIt].data, vertices, currVtxOffset);

                // indices
                ProcessIndices(model, *prim.indices, indices, currIdxOffset);

                // TEXCOORD_0
                if (texIt != -1)
                {
                    ProcessTexCoords(model, *prim.attributes[texIt].data, vertices, currVtxOffset);

                    // If vertex tangents aren't present, compute them. Make sure the computation 
                    // happens after vertex and index processing.
                    if (tangentIt != -1)
                        ProcessTangents(model, *prim.attributes[tangentIt].data, vertices, currVtxOffset);
                    else
                    {
                        Math::ComputeMeshTangentVectors(MutableSpan(vertices.begin() + currVtxOffset, numVertices),
                            Span(indices.begin() + currIdxOffset, numIndices),
                            false);
                    }
                }

                meshes[currMeshPrimOffset++] = Mesh
                    {
                        .SceneID = sceneID,
                        .glTFMaterialIdx = prim.material ? (int)(prim.material - model.materials) : -1,
                        .MeshIdx = (int)meshIdx,
                        .MeshPrimIdx = primIdx,
                        .BaseVtxOffset = currVtxOffset,
                        .BaseIdxOffset = currIdxOffset,
                        .NumVertices = numVertices,
                        .NumIndices = numIndices
                    };

                // Remember every mesh with an emissive material assigned to it.
                if (prim.material)
                {
                    float emissiveFactDot1 = prim.material->emissive_factor[0] + 
                        prim.material->emissive_factor[1] + 
                        prim.material->emissive_factor[2];

                    if ((emissiveFactDot1 > 0 || prim.material->has_emissive_strength || 
                        prim.material->emissive_texture.texture))
                    {
                        const uint64_t meshID = Scene::MeshID(sceneID, (int)meshIdx, primIdx);

                        emissivesPrims[workerBaseEmissiveOffset + numEmissiveMeshPrims++] = EmissiveMeshPrim
                            {
                                .MeshID = meshID,
                                .BaseVtxOffset = currVtxOffset,
                                .BaseIdxOffset = currIdxOffset,
                                .NumIndices = numIndices,
                                .MaterialIdx = (int)(prim.material - model.materials)
                            };
                    }
                }

                currVtxOffset += numVertices;
                currIdxOffset += numIndices;
            }
        }

        emissivePrimCount = numEmissiveMeshPrims;
    }

    void LoadDDSImages(uint32_t sceneID, const Filesystem::Path& modelDir, const cgltf_data& model,
        size_t offset, size_t size, MutableSpan<DDSImage> ddsImages)
    {
        // For loading DDS data from disk
        MemoryArena memArena(64 * 1024 * 1024);
        // For uploading texture to GPU 
        UploadHeapArena heapArena(64 * 1024 * 1024);

        char ext[8];

        for (size_t m = offset; m != offset + size; m++)
        {
            const cgltf_image& image = model.images[m];
            if (image.uri)
            {
                Filesystem::Path p(modelDir.GetView());
                p.Append(image.uri);
                p.Extension(ext);

                if (strcmp(ext, "dds") != 0)
                    continue;

                const uint64_t id = XXH3_64bits(p.Get(), p.Length());
                Texture tex;
                auto err = GpuMemory::GetTexture2DFromDisk(p, tex, heapArena, ArenaAllocator(memArena));

                if (err != LOAD_DDS_RESULT::SUCCESS)
                {
                    if (err == LOAD_DDS_RESULT::FILE_NOT_FOUND)
                    {
                        LOG_UI_WARNING(
                            "Texture in path %s was present in the glTF scene file, but wasn't found on disk. Skipping...\n", 
                            p.Get());
                        continue;
                    }
                    
                    Check(false, "Error while loading DDS texture in path %s: %d", p.Get(), err);
                }

                ddsImages[m] = DDSImage{ .T = ZetaMove(tex), .ID = id };
            }
        }
    }

    void ProcessMaterials(uint32_t sceneID, const Filesystem::Path& modelDir, const cgltf_data& model,
        int offset, int size, const MutableSpan<DDSImage> ddsImages)
    {
        auto getAlphaMode = [](cgltf_alpha_mode m)
            {
                switch (m)
                {
                case cgltf_alpha_mode_opaque:
                    return Material::ALPHA_MODE::OPAQUE_;
                case cgltf_alpha_mode_mask:
                    return Material::ALPHA_MODE::MASK;
                case cgltf_alpha_mode_blend:
                    return Material::ALPHA_MODE::BLEND;
                default:
                    break;
                }

                Assert(false, "invalid alpha mode.");
                return Material::ALPHA_MODE::OPAQUE_;
            };

        for (int m = offset; m != offset + size; m++)
        {
            const auto& mat = model.materials[m];
            Check(mat.has_pbr_metallic_roughness, "Material is not supported.");

            glTF::Asset::MaterialDesc desc;
            desc.ID = Scene::MaterialID(sceneID, m);
            desc.AlphaMode = getAlphaMode(mat.alpha_mode);
            desc.AlphaCutoff = (float)mat.alpha_cutoff;
            desc.DoubleSided = mat.double_sided;

            // Base Color map
            {
                const cgltf_texture_view& baseColView = mat.pbr_metallic_roughness.base_color_texture;
                if (baseColView.texture)
                {
                    Check(baseColView.texture->image, "textureView doesn't point to any image.");

                    Filesystem::Path p(modelDir.GetView());
                    p.Append(baseColView.texture->image->uri);

                    desc.BaseColorTexPath = XXH3_64bits(p.Get(), p.Length());
                }

                auto& f = mat.pbr_metallic_roughness.base_color_factor;
                desc.BaseColorFactor = float4(f[0], f[1], f[2], f[3]);
            }

            // Normal map
            {
                const cgltf_texture_view& normalView = mat.normal_texture;
                if (normalView.texture)
                {
                    Check(normalView.texture->image, "textureView doesn't point to any image.");
                    const char* texPath = normalView.texture->image->uri;

                    Filesystem::Path p(modelDir.GetView());
                    p.Append(normalView.texture->image->uri);
                    desc.NormalTexPath = XXH3_64bits(p.Get(), p.Length());

                    desc.NormalScale = (float)mat.normal_texture.scale;
                }
            }

            // Metallic-Roughness map
            {
                const cgltf_texture_view& metallicRoughnessView = mat.pbr_metallic_roughness.metallic_roughness_texture;
                if (metallicRoughnessView.texture)
                {
                    Check(metallicRoughnessView.texture->image, 
                        "textureView doesn't point to any image.");

                    Filesystem::Path p(modelDir.GetView());
                    p.Append(metallicRoughnessView.texture->image->uri);
                    desc.MetallicRoughnessTexPath = XXH3_64bits(p.Get(), p.Length());
                }

                desc.MetallicFactor = (float)mat.pbr_metallic_roughness.metallic_factor;
                desc.SpecularRoughnessFactor = (float)mat.pbr_metallic_roughness.roughness_factor;
            }

            // Emissive map
            {
                const cgltf_texture_view& emissiveView = mat.emissive_texture;
                if (emissiveView.texture)
                {
                    Check(emissiveView.texture->image, "textureView doesn't point to any image.");
                    const char* texPath = emissiveView.texture->image->uri;

                    Filesystem::Path p(modelDir.GetView());
                    p.Append(emissiveView.texture->image->uri);
                    desc.EmissiveTexPath = XXH3_64bits(p.Get(), p.Length());
                }

                auto& f = mat.emissive_factor;
                desc.EmissiveFactor = float3((float)f[0], (float)f[1], (float)f[2]);

                if (mat.has_emissive_strength)
                    desc.EmissiveStrength = mat.emissive_strength.emissive_strength;
            }

            if (mat.has_ior)
                desc.SpecularIOR = mat.ior.ior;
            if (mat.has_transmission)
                desc.TransmissionWeight = mat.transmission.transmission_factor;
            if (mat.has_clearcoat)
            {
                desc.CoatWeight = mat.clearcoat.clearcoat_factor;
                desc.CoatRoughness = mat.clearcoat.clearcoat_roughness_factor;
            }

            SceneCore& scene = App::GetScene();
            scene.AddMaterial(desc, ddsImages, false);
        }
    }

    void NumEmissiveInstancesAndTrianglesSubtree(const cgltf_node& node, ThreadContext& context)
    {
        if (node.mesh)
        {
            const int meshIdx = (int)(node.mesh - context.Model->meshes);

            // A separate instance for each primitive
            for (int primIdx = 0; primIdx < node.mesh->primitives_count; primIdx++)
            {
                const cgltf_primitive& meshPrim = node.mesh->primitives[primIdx];

                if (meshPrim.material)
                {
                    const uint64_t meshID = Scene::MeshID(context.SceneID, (int)meshIdx, primIdx);
                    const auto idx = BinarySearch(Span(context.EmissiveMeshPrims), meshID, 
                        [](const EmissiveMeshPrim& p) {return p.MeshID; });

                    // Does this mesh prim have an emissive material assigned to it?
                    if (idx != -1)
                    {
                        const auto& meshPrimInfo = context.EmissiveMeshPrims[idx];
                        Assert(meshPrimInfo.MaterialIdx == (int)(meshPrim.material - context.Model->materials), 
                            "Material index mismatch.");

                        context.NumEmissiveTris += meshPrimInfo.NumIndices / 3;
                        context.NumEmissiveInstances++;
                    }
                }
            }
        }

        for (int c = 0; c < node.children_count; c++)
        {
            const cgltf_node& childNode = *node.children[c];
            NumEmissiveInstancesAndTrianglesSubtree(childNode, context);
        }
    }

    void NumEmissiveInstancesAndTriangles(ThreadContext& context)
    {
        for (size_t i = 0; i < context.Model->scene->nodes_count; i++)
        {
            const cgltf_node& node = *context.Model->scene->nodes[i];
            NumEmissiveInstancesAndTrianglesSubtree(node, context);
        }
    }

    void ProcessEmissiveSubtree(const cgltf_node& node, ThreadContext& context, int& emissiveMeshIdx,
        uint32_t& rtEmissiveTriIdx)
    {
        SceneCore& scene = App::GetScene();
        uint32_t currGlobalTriIdx = rtEmissiveTriIdx;

        if (node.mesh)
        {
            const int meshIdx = (int)(node.mesh - context.Model->meshes);

            // A separate instance for each primitive
            for (int primIdx = 0; primIdx < node.mesh->primitives_count; primIdx++)
            {
                const cgltf_primitive& meshPrim = node.mesh->primitives[primIdx];

                if (meshPrim.material)
                {
                    const uint64_t meshID = Scene::MeshID(context.SceneID, (int)meshIdx, primIdx);
                    const auto idx = BinarySearch(Span(context.EmissiveMeshPrims), meshID, 
                        [](const EmissiveMeshPrim& p) {return p.MeshID; });

                    if (idx != -1)
                    {
                        uint32_t emissiveFactorRGB = Float3ToRGB8(float3(meshPrim.material->emissive_factor[0],
                            meshPrim.material->emissive_factor[1],
                            meshPrim.material->emissive_factor[2]));

                        const auto& meshPrimInfo = context.EmissiveMeshPrims[idx];

                        const uint32_t matID = Scene::MaterialID(context.SceneID, meshPrimInfo.MaterialIdx);
                        const Material* mat = scene.GetMaterial(matID).value();

                        const int nodeIdx = (int)(&node - context.Model->nodes);
                        const uint64_t currInstanceID = Scene::InstanceID(context.SceneID, nodeIdx, meshIdx, primIdx);

                        // Add emissive instance
                        context.EmissiveInstances[emissiveMeshIdx++] = EmissiveInstance
                            {
                                .InstanceID = currInstanceID,
                                .BaseTriOffset = currGlobalTriIdx,
                                .NumTriangles = meshPrimInfo.NumIndices / 3,
                                .MaterialIdx = meshPrimInfo.MaterialIdx + 1
                            };

                        uint32_t currMeshTriIdx = 0;

                        // Add all triangles for this instance
                        for (int i = meshPrimInfo.BaseIdxOffset; i < (int)(meshPrimInfo.BaseIdxOffset + meshPrimInfo.NumIndices); i += 3)
                        {
                            uint32_t i0 = context.Indices[i];
                            uint32_t i1 = context.Indices[i + 1];
                            uint32_t i2 = context.Indices[i + 2];

                            const Vertex& v0 = context.Vertices[meshPrimInfo.BaseVtxOffset + i0];
                            const Vertex& v1 = context.Vertices[meshPrimInfo.BaseVtxOffset + i1];
                            const Vertex& v2 = context.Vertices[meshPrimInfo.BaseVtxOffset + i2];

                            context.RTEmissives[currGlobalTriIdx++] = RT::EmissiveTriangle(
                                v0.Position, v1.Position, v2.Position,
                                v0.TexUV, v1.TexUV, v2.TexUV,
                                emissiveFactorRGB, mat->GetEmissiveTex(), mat->GetEmissiveStrength(),
                                currMeshTriIdx++, mat->DoubleSided());
                        }
                    }
                }
            }
        }

        rtEmissiveTriIdx = currGlobalTriIdx;

        for (int c = 0; c < node.children_count; c++)
        {
            const cgltf_node& childNode = *node.children[c];
            ProcessEmissiveSubtree(childNode, context, emissiveMeshIdx, rtEmissiveTriIdx);
        }
    }

    void ProcessEmissives(ThreadContext& context)
    {
        int emissiveMeshIdx = 0;
        uint32_t rtEmissiveTriIdx = 0;

        for (size_t i = 0; i < context.Model->scene->nodes_count; i++)
        {
            const cgltf_node& node = *context.Model->scene->nodes[i];
            ProcessEmissiveSubtree(node, context, emissiveMeshIdx, rtEmissiveTriIdx);
        }

        Assert(emissiveMeshIdx == context.NumEmissiveInstances, "these must match.");
        Assert(rtEmissiveTriIdx == context.NumEmissiveTris, "these must match.");
    }

    void ProcessNodeSubtree(const cgltf_node& node, uint32_t sceneID, const cgltf_data& model,
        uint64_t parentId)
    {
        uint64_t currInstanceID = SceneCore::ROOT_ID;

        AffineTransformation transform = AffineTransformation::GetIdentity();

        if (node.has_matrix)
        {
            float4x4a M(node.matrix);
            v_float4x4 vM = load4x4(M);
            auto det = store(det3x3(vM));
            //Check(fabsf(det.x) > 1e-6f, "Transformation matrix with a zero determinant is invalid.");
            Check(det.x > 0.0f, 
                "Transformation matrices that change the orientation (e.g. negative scaling) are not supported.");

            // Column-major storage to row-major storage
            vM = transpose(vM);
            M = store(vM);

            // To apply the transformation matrix M = [u v w] from the RHS coordinate system (+Y up) 
            // to some vector x in the LHS system (+Y up), let C denote the change-of-basis transformation 
            // matrix from the latter to the former. The transformation of x (denote by x') is given by
            //
            //      x' = C^-1 M C x.
            // 
            // Replacing C in above
            //
            //           | 1  0  0 |         | 1  0  0 |
            //      x' = | 0  1  0 | [u v w] | 0  1  0 | x
            //           | 0  0 -1 |         | 0  0 -1 |
            //
            //           | 1  0  0 |
            //         = | 0  1  0 | [u v -w] x
            //           | 0  0 -1 |
            //
            //           |  u_1  v_1  -w_1 |
            //         = |  u_2  v_2  -w_2 | x
            //           | -u_3 -v_3   w_3 |
            //
            M.m[0].z *= -1.0f;
            M.m[1].z *= -1.0f;
            M.m[2].x *= -1.0f;
            M.m[2].y *= -1.0f;

            // Convert translation to LHS
            M.m[2].w *= -1.0f;

            vM = load4x4(M);
            decomposeTRS(vM, transform.Scale, transform.Rotation, transform.Translation);
        }
        else
        {
            if (node.has_scale)
            {
                Check(node.scale[0] > 0 && node.scale[1] > 0 && node.scale[2] > 0, 
                    "Negative scale factors are not supported.");
                transform.Scale = float3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);
            }

            if (node.has_translation)
            {
                transform.Translation = float3((float)node.translation[0], (float)node.translation[1], 
                    (float)-node.translation[2]);
            }

            if (node.has_rotation)
            {
                // Rotation quaternion q = (n_x * s, n_y * s, n_z * s, c)
                // where s = sin(theta/2) and c = cos(theta/2).
                //
                // In the LHS system (+Y up), n_lhs = (n_x, n_y, -n_z)
                // and theta_lhs = -theta. Since sin(-a) = -sin(a) and cos(-a) = cos(a)
                //
                //        q_lhs = (n_x * -s, n_y * -s, -n_z * -s, c)
                //              = (-n_x * s, -n_y * s, n_z * s, c)
                //
                transform.Rotation = float4(-(float)node.rotation[0],
                    -(float)node.rotation[1],
                    (float)node.rotation[2],
                    (float)node.rotation[3]);

#if CHECK_QUATERNION_VALID == 1
                // check ||quaternion|| = 1
                __m128 vV = _mm_loadu_ps(&transform.Rotation.x);
                __m128 vLength = _mm_dp_ps(vV, vV, 0xff);
                vLength = _mm_sqrt_ps(vLength);
                __m128 vOne = _mm_set1_ps(1.0f);
                __m128 vDiff = _mm_sub_ps(vLength, vOne);
                float d = _mm_cvtss_f32(abs(vDiff));
                Check(d < 1e-6f, "Invalid rotation quaternion.");
#endif
            }
        }

        // Workaround for nodes without a name
        const int nodeIdx = (int)(&node - model.nodes);
        Assert(nodeIdx < model.nodes_count, "Invalid node index.");

        if (node.mesh)
        {
            const int meshIdx = (int)(node.mesh - model.meshes);
            Assert(meshIdx < model.meshes_count, "Invalid mesh index.");

            // A separate instance for each primitive
            for (int primIdx = 0; primIdx < node.mesh->primitives_count; primIdx++)
            {
                const cgltf_primitive& meshPrim = node.mesh->primitives[primIdx];
                float emissiveFactDot1 = 0;

                if (meshPrim.material)
                {
                    emissiveFactDot1 = meshPrim.material->emissive_factor[0] +
                        meshPrim.material->emissive_factor[1] +
                        meshPrim.material->emissive_factor[2];
                }

                const uint8_t rtInsMask = meshPrim.material &&
                    (meshPrim.material->emissive_texture.texture || (emissiveFactDot1 > 0)) ?
                    RT_AS_SUBGROUP::EMISSIVE :
                    RT_AS_SUBGROUP::NON_EMISSIVE;

                // Parent-child relationships will be w.r.t. the last mesh primitive
                currInstanceID = Scene::InstanceID(sceneID, nodeIdx, meshIdx, primIdx);

                const bool isOpaque = meshPrim.material && meshPrim.material->alpha_mode != cgltf_alpha_mode_opaque ?
                    false :
                    true;

                glTF::Asset::InstanceDesc desc{
                    .LocalTransform = transform,
                    .SceneID = sceneID,
                    .ID = currInstanceID,
                    .ParentID = parentId,
                    .MeshIdx = meshIdx,
                    .MeshPrimIdx = primIdx,
                    .RtMeshMode = RT_MESH_MODE::STATIC,
                    .RtInstanceMask = rtInsMask,
                    .IsOpaque = isOpaque };

                SceneCore& scene = App::GetScene();
                scene.AddInstance(desc, false);
            }
        }
        else
        {
            currInstanceID = Scene::InstanceID(sceneID, nodeIdx, -1, -1);

            glTF::Asset::InstanceDesc desc{
                .LocalTransform = transform,
                    .SceneID = sceneID,
                    .ID = currInstanceID,
                    .ParentID = parentId,
                    .MeshIdx = -1,
                    .MeshPrimIdx = -1,
                    .RtMeshMode = RT_MESH_MODE::STATIC,
                    .RtInstanceMask = RT_AS_SUBGROUP::NON_EMISSIVE };

            SceneCore& scene = App::GetScene();
            scene.AddInstance(desc, false);
        }

        for (int c = 0; c < node.children_count; c++)
        {
            const cgltf_node& childNode = *node.children[c];
            ProcessNodeSubtree(childNode, sceneID, model, currInstanceID);
        }
    }

    void ProcessNodes(const cgltf_data& model, uint32_t sceneID)
    {
        for (size_t i = 0; i < model.scene->nodes_count; i++)
        {
            const cgltf_node& node = *model.scene->nodes[i];
            ProcessNodeSubtree(node, sceneID, model, SceneCore::ROOT_ID);
        }
    }

    void DescendTree(const cgltf_node& node, int height, Vector<int>& treeLevels)
    {
        // Some meshes can have multiple mesh primitives, each one is treated as a separate
        // instance here
        treeLevels[height] += node.mesh ? (int)node.mesh->primitives_count : 1;

        for (size_t i = 0; i < node.children_count; i++)
        {
            const cgltf_node& childNode = *node.children[i];
            DescendTree(childNode, height + 1, treeLevels);
        }
    }

    void PrecomputeNodeHierarchy(const cgltf_data& model, Vector<int>& treeLevels)
    {
        // NOTE model.scene->nodes refers to first-level nodes only
        for (size_t i = 0; i < model.scene->nodes_count; i++)
        {
            const cgltf_node& firstLevelNode = *model.scene->nodes[i];
            treeLevels[0] += firstLevelNode.mesh ? (int)firstLevelNode.mesh->primitives_count : 1;
        }

        for (size_t i = 0; i < model.scene->nodes_count; i++)
        {
            const cgltf_node& firstLevelNode = *model.scene->nodes[i];
            if (firstLevelNode.children_count)
            {
                for (size_t j = 0; j < firstLevelNode.children_count; j++)
                {
                    const cgltf_node& childNode = *firstLevelNode.children[j];
                    DescendTree(childNode, 1, treeLevels);
                }
            }
        }
    }

    int TreeHeight(const cgltf_node& node)
    {
        int height = 0;

        for (size_t i = 0; i < node.children_count; i++)
        {
            const cgltf_node& childNode = *node.children[i];
            height = Max(height, TreeHeight(childNode) + 1);
        }

        return height;
    }

    int ComputeNodeHierarchyHeight(const cgltf_data& model)
    {
        int height = 0;

        for (size_t i = 0; i < model.scene->nodes_count; i++)
        {
            const cgltf_node& childNode = *model.scene->nodes[i];
            height = Max(height, TreeHeight(childNode) + 1);
        }

        return height;
    }

    void TotalNumVerticesAndIndices(cgltf_data* model, size_t& numVertices, size_t& numIndices, 
        size_t& numMeshes)
    {
        numVertices = 0;
        numIndices = 0;
        numMeshes = 0;

        for (size_t meshIdx = 0; meshIdx != model->meshes_count; meshIdx++)
        {
            const auto& mesh = model->meshes[meshIdx];
            numMeshes += mesh.primitives_count;

            for (size_t primIdx = 0; primIdx < mesh.primitives_count; primIdx++)
            {
                const auto& prim = mesh.primitives[primIdx];

                if (prim.type != cgltf_primitive_type_triangles)
                    continue;

                for (int attrib = 0; attrib < prim.attributes_count; attrib++)
                {
                    if (strcmp("POSITION", prim.attributes[attrib].name) == 0)
                    {
                        auto& accessor = prim.attributes[attrib].data;
                        numVertices += accessor->count;

                        break;
                    }
                }

                numIndices += prim.indices->count;
            }
        }
    }
}

void glTF::Load(const App::Filesystem::Path& pathToglTF)
{
    // Parse json
    cgltf_options options{};
    cgltf_data* model = nullptr;
    Checkgltf(cgltf_parse_file(&options, pathToglTF.GetView().data(), &model));

    //Check(model->extensions_required_count == 0, "Required glTF extensions are not supported.");

    // Load buffers
    Check(model->buffers_count == 1, "Invalid number of buffers");
    Filesystem::Path bufferPath(pathToglTF.GetView());
    bufferPath.Directory();
    bufferPath.Append(model->buffers[0].uri);
    Checkgltf(cgltf_load_buffers(&options, model, bufferPath.Get()));

    Check(model->scene, "No scene found in glTF file: %s.", pathToglTF.GetView());
    const uint32_t sceneID = XXH3_64_To_32(XXH3_64bits(pathToglTF.GetView().data(), pathToglTF.Length()));
    SceneCore& scene = App::GetScene();

    // All the unique textures that need to be loaded from disk
    SmallVector<DDSImage> ddsImages;
    ddsImages.resize(model->images_count);

    // Figure out total number of vertices and indices
    size_t totalNumVertices;
    size_t totalNumIndices;
    size_t totalNumMeshPrims;
    TotalNumVerticesAndIndices(model, totalNumVertices, totalNumIndices, totalNumMeshPrims);

    // Height of the node hierarchy
    const int height = ComputeNodeHierarchyHeight(*model);
    constexpr int DEFAULT_NUM_LEVELS = 10;
    SmallVector<int, SystemAllocator, DEFAULT_NUM_LEVELS> levels;
    levels.resize(height, 0);

    // Precompute number of nodes per level
    PrecomputeNodeHierarchy(*model, levels);

    size_t total = 0;
    for (size_t i = 0; i < levels.size(); i++)
        total += levels[i];

    // Preallocate
    SmallVector<Core::Vertex> vertices;
    SmallVector<uint32_t> indices;
    SmallVector<Mesh> meshes;
    SmallVector<EmissiveMeshPrim> emissivePrims;
    SmallVector<RT::EmissiveTriangle> rtEmissives;
    SmallVector<EmissiveInstance> emissiveInstances;

    vertices.resize(totalNumVertices);
    indices.resize(totalNumIndices);
    meshes.resize(totalNumMeshPrims);
    emissivePrims.resize(totalNumMeshPrims);
    ResetEmissiveSubsets(emissivePrims);
    scene.ResizeAdditionalMaterials((uint32_t)model->materials_count);
    scene.ReserveInstances(levels, total);

    // How many meshes are processed by each worker
    constexpr size_t MAX_NUM_MESH_WORKERS = 4;
    constexpr size_t MIN_MESHES_PER_WORKER = 20;
    size_t meshWorkerOffset[MAX_NUM_MESH_WORKERS];
    size_t meshWorkerCount[MAX_NUM_MESH_WORKERS];
    uint32_t workerEmissiveCount[MAX_NUM_MESH_WORKERS];

    const int numMeshWorkers = (int)SubdivideRangeWithMin(model->meshes_count,
        MAX_NUM_MESH_WORKERS,
        meshWorkerOffset,
        meshWorkerCount,
        MIN_MESHES_PER_WORKER);

    // How many images are processed by each worker
    constexpr size_t MAX_NUM_IMAGE_WORKERS = 5;
    constexpr size_t MIN_IMAGES_PER_WORKER = 15;
    size_t imgWorkerOffset[MAX_NUM_IMAGE_WORKERS];
    size_t imgWorkerCount[MAX_NUM_IMAGE_WORKERS];

    const int numImgWorkers = (int)SubdivideRangeWithMin(model->images_count,
        MAX_NUM_IMAGE_WORKERS,
        imgWorkerOffset,
        imgWorkerCount,
        MIN_IMAGES_PER_WORKER);

    std::atomic_uint32_t currVtxOffset = 0;
    std::atomic_uint32_t currIdxOffset = 0;
    std::atomic_uint32_t currMeshPrimOffset = 0;

    ThreadContext tc{ .SceneID = sceneID,
        .Path = pathToglTF,
        .Model = model,
        .NumMeshWorkers = numMeshWorkers,
        .NumImgWorkers = numImgWorkers,
        .MeshThreadOffsets = meshWorkerOffset, .MeshThreadSizes = meshWorkerCount,
        .ImgThreadOffsets = imgWorkerOffset, .ImgThreadSizes = imgWorkerCount,
        .Vertices = vertices,
        .CurrVtxOffset = currVtxOffset,
        .Indices = indices,
        .CurrIdxOffset = currIdxOffset,
        .Meshes = meshes,
        .CurrMeshPrimOffset = currMeshPrimOffset,
        .EmissiveMeshPrims = emissivePrims,
        .EmissiveMeshPrimCountPerWorker = workerEmissiveCount,
        .RTEmissives = rtEmissives,
        .EmissiveInstances = emissiveInstances,
        .NumEmissiveMeshPrims = 0,
        .NumEmissiveInstances = 0,
        .NumEmissiveTris = 0
    };

    TaskSet ts;

    auto procEmissiveMeshPrims = ts.EmplaceTask("gltf::EmissivePrims", [&tc]()
        {
            // EmissiveMeshPrimCountPerWorker is filled in by mesh workers
            for (int i = 0; i < tc.NumMeshWorkers; i++)
                tc.NumEmissiveMeshPrims += tc.EmissiveMeshPrimCountPerWorker[i];

            // For binary search. Also, since non-emissive meshes were assigned the INVALID
            // ID (= UINT64_MAX), this also partitions the non-null entries before the null
            // entries.
            std::sort(tc.EmissiveMeshPrims.begin(), tc.EmissiveMeshPrims.end(),
                [](const EmissiveMeshPrim& lhs, const EmissiveMeshPrim& rhs)
                {
                    return lhs.MeshID < rhs.MeshID;
                });

            // In order to do only one allocation, number of emissive mesh primitives was assumed
            // to be the worst case -- total number of mesh primitives. As such, there may be a number 
            // of "null" entries in the EmissiveMeshPrims. Now that the actual size is known, adjust 
            // the Span range accordingly.
            tc.EmissiveMeshPrims = MutableSpan(tc.EmissiveMeshPrims.data(), tc.NumEmissiveMeshPrims);
            NumEmissiveInstancesAndTriangles(tc);
        });

    for (int i = 0; i < tc.NumMeshWorkers; i++)
    {
        StackStr(tname, n, "gltf::Mesh_%d", i);

        auto procMesh = ts.EmplaceTask(tname, [&tc, workerIdx = i]()
            {
                ProcessMeshes(*tc.Model, tc.SceneID, tc.MeshThreadOffsets[workerIdx],
                    tc.MeshThreadSizes[workerIdx],
                    tc.Vertices, tc.CurrVtxOffset,
                    tc.Indices, tc.CurrIdxOffset,
                    tc.Meshes, tc.CurrMeshPrimOffset,
                    tc.EmissiveMeshPrims, tc.EmissiveMeshPrimCountPerWorker[workerIdx]);
            });

        ts.AddOutgoingEdge(procMesh, procEmissiveMeshPrims);
    }

    auto procMats = ts.EmplaceTask("gltf::Materials", [&tc, &ddsImages]()
        {
            // For binary search
            std::sort(ddsImages.begin(), ddsImages.end(), 
                [](const DDSImage& lhs, const DDSImage& rhs)
                {
                    return lhs.ID < rhs.ID;
                });

            Filesystem::Path parent(tc.Path.GetView());
            parent.ToParent();

            ProcessMaterials(tc.SceneID, parent, *tc.Model, 0, (int)tc.Model->materials_count, 
                ddsImages);
        });

    for (int i = 0; i < numImgWorkers; i++)
    {
        StackStr(tname, n, "gltf::Img_%d", i);

        // Loads dds textures from disk and upload them to GPU
        auto h = ts.EmplaceTask(tname, [&pathToglTF, &ddsImages, &tc, workerIdx = i]()
            {
                Filesystem::Path parent(pathToglTF.GetView());
                parent.ToParent();

                LoadDDSImages(tc.SceneID, parent, *tc.Model, tc.ImgThreadOffsets[workerIdx], 
                    tc.ImgThreadSizes[workerIdx], ddsImages);
            });

        // Material processing should start after textures are loaded
        ts.AddOutgoingEdge(h, procMats);
    }

    // For each node with an emissive mesh primitive, add all of its triangles to 
    // the emissive buffer
    auto procEmissives = ts.EmplaceTask("gltf::Emissives", [&tc]()
        {
            tc.EmissiveInstances.resize(tc.NumEmissiveInstances);
            tc.RTEmissives.resize(tc.NumEmissiveTris);

            ProcessEmissives(tc);

            // Transfer ownership of emissives
            SceneCore& scene = App::GetScene();
            scene.AddEmissives(ZetaMove(tc.EmissiveInstances), ZetaMove(tc.RTEmissives), false);
        });

    // Processing emissives starts after materials are loaded and emissive primitives 
    // have been processed
    ts.AddOutgoingEdge(procEmissiveMeshPrims, procEmissives);
    ts.AddOutgoingEdge(procMats, procEmissives);

    auto procNodes = ts.EmplaceTask("gltf::Nodes", [&tc]()
        {
            ProcessNodes(*tc.Model, tc.SceneID);
        });

    auto last = ts.EmplaceTask("gltf::Final", [&tc]()
        {
            // Transfer ownership of mesh buffers
            SceneCore& scene = App::GetScene();
            scene.AddMeshes(ZetaMove(tc.Meshes), ZetaMove(tc.Vertices), ZetaMove(tc.Indices), false);

            cgltf_free(tc.Model);
        });

    // Final task has to run after all the other tasks
    ts.AddIncomingEdgeFromAll(last);

    WaitObject waitObj;
    ts.Sort();
    ts.Finalize(&waitObj);
    App::Submit(ZetaMove(ts));

    // Help out with unfinished tasks. Note: This thread might help
    // with tasks that are not related to loading glTF.
    App::FlushWorkerThreadPool();
    waitObj.Wait();
}
