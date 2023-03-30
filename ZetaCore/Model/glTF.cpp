#include "glTF.h"
#include "glTFAsset.h"
#include "../Math/MatrixFuncs.h"
#include "../Math/Surface.h"
#include "../Math/Quaternion.h"
#include "../Scene/SceneCore.h"
#include "../RayTracing/RtCommon.h"
#include "../Support/Task.h"

#define JSON_NOEXCEPTION
#define JSON_NO_IO
#include <tinyglTF/json.hpp>
#define TINYGLTF_NOEXCEPTION
#define TINYGLTF_NO_STB_IMAGE 
#define TINYGLTF_NO_STB_IMAGE_WRITE 
#define TINYGLTF_NO_EXTERNAL_IMAGE  
#define TINYGLTF_NO_INCLUDE_STB_IMAGE   
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE    
#define TINYGLTF_USE_CPP14     
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_RAPIDJSON
#define TINYGLTF_IMPLEMENTATION
#include <tinyglTF/tiny_gltf.h>

using namespace ZetaRay;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;
using namespace ZetaRay::Core;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Model;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// glTF
//--------------------------------------------------------------------------------------

namespace
{
	uint64_t MeshID(uint64_t sceneID, int meshIdx, int meshPrimIdx) noexcept
	{
		StackStr(str, n, "mesh_%llu_%d_%d", sceneID, meshIdx, meshPrimIdx);
		uint64_t meshFromSceneID = XXH3_64bits(str, n);

		return meshFromSceneID;
	}

	struct IntemediateInstance
	{
		AffineTransformation LocalTransform;
		int MeshIdx;
		std::string_view Name;
		uint64_t ParentID;
	};

	void ProcessPositions(const tinygltf::Model& model, int posIdx, Span<Vertex> vertices) noexcept
	{
		const auto& accessor = model.accessors[posIdx];

		Check(accessor.type == TINYGLTF_TYPE_VEC3, "Invalid type for POSITION attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for POSITION attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride == sizeof(float3), "Invalid stride for POSITION attribute.");

		const auto& buffer = model.buffers[bufferView.buffer];
		const float3* start = (float3*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float3* curr = start + i;

			// glTF uses a right-handed coordinate system with +Y as up
			vertices[i].Position = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessNormals(const tinygltf::Model& model, int normalIdx, Span<Vertex> vertices) noexcept
	{
		const auto& accessor = model.accessors[normalIdx];

		Check(accessor.type == TINYGLTF_TYPE_VEC3, "Invalid type for NORMAL attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for NORMAL attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride == sizeof(float3), "Invalid stride for NORMAL attribute.");

		const auto& buffer = model.buffers[bufferView.buffer];
		const float3* start = (float3*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float3* curr = start + i;

			// glTF uses a right-handed coordinate system with +Y as up
			vertices[i].Normal = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessTexCoords(const tinygltf::Model& model, int texCoord0Idx, Span<Vertex> vertices) noexcept
	{
		const auto& accessor = model.accessors[texCoord0Idx];

		Check(accessor.type == TINYGLTF_TYPE_VEC2, "Invalid type for TEXCOORD_0 attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for TEXCOORD_0 attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride == sizeof(float2), "Invalid stride for TEXCOORD_0 attribute.");

		const auto& buffer = model.buffers[bufferView.buffer];
		const float2* start = (float2*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float2* curr = start + i;
			vertices[i].TexUV = float2(curr->x, curr->y);
		}
	}

	void ProcessTangents(const tinygltf::Model& model, int tangentIdx, Span<Vertex> vertices) noexcept
	{
		const auto& accessor = model.accessors[tangentIdx];

		Check(accessor.type == TINYGLTF_TYPE_VEC3 || accessor.type == TINYGLTF_TYPE_VEC4, "Invalid type for TANGENT attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for TANGENT attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);

		const auto& buffer = model.buffers[bufferView.buffer];
		const float4* start = (float4*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float4* curr = start + i;

			// glTF uses a right-handed coordinate system with +Y as up
			vertices[i].Tangent = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessIndices(const tinygltf::Model& model, int indicesIdx, Vector<uint32_t, App::ThreadAllocator>& indices) noexcept
	{
		const auto& accessor = model.accessors[indicesIdx];
		Check(accessor.type == TINYGLTF_TYPE_SCALAR, "Invalid index type.");

		auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride != -1, "Invalid index stride.");

		auto& buffer = const_cast<tinygltf::Model&>(model).buffers[bufferView.buffer];
		indices.reserve(accessor.count);

		// populate the mesh indices
		uint8_t* curr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
		Check(accessor.count % 3 == 0, "invalid number of indices");
		const size_t numFaces = accessor.count / 3;

		for (size_t face = 0; face < numFaces; face++)
		{
			uint32_t i0 = 0;
			uint32_t i1 = 0;
			uint32_t i2 = 0;

			memcpy(&i0, curr, byteStride);
			curr += byteStride;
			memcpy(&i1, curr, byteStride);
			curr += byteStride;
			memcpy(&i2, curr, byteStride);
			curr += byteStride;

			indices.push_back(i0);
			indices.push_back(i2);
			indices.push_back(i1);
		}
	}

	void ProcessMeshes(uint64_t sceneID, tinygltf::Model& model, size_t offset, size_t size) noexcept
	{
		SceneCore& scene = App::GetScene();

		for (size_t meshIdx = offset; meshIdx != offset + size; meshIdx++)
		{
			Assert(meshIdx < model.meshes.size(), "out-of-bound access");
			const auto& mesh = model.meshes[meshIdx];
			int primIdx = 0;

			// fill in the subsets
			for (const auto& prim : mesh.primitives)
			{
				glTF::Asset::MeshSubset subset;
				subset.MeshIdx = (int)meshIdx;
				subset.MeshPrimIdx = primIdx;

				Check(prim.indices != -1, "index buffer is required.");
				Check(prim.mode == TINYGLTF_MODE_TRIANGLES, "Non-triangle meshes are not supported.");

				auto posIt = prim.attributes.find("POSITION");

				// workaround for weird bug when /fsanitize=address is used -- for some
				// reason "POSITION" becomes "POSITIONsssss"
				if (posIt == prim.attributes.end())
				{
					for (auto it = prim.attributes.begin(); it != prim.attributes.end(); it++)
					{
						if (it->first.starts_with("POSITION"))
						{
							posIt = it;
							break;
						}
					}
				}

				Check(posIt != prim.attributes.end(), "POSITION was not found in the vertex attributes.");

				auto normalit = prim.attributes.find("NORMAL");
				Check(normalit != prim.attributes.end(), "NORMAL was not found in the vertex attributes.");

				auto texIt = prim.attributes.find("TEXCOORD_0");

				// workaround for weird bug when /fsanitize=address is used -- for some
				// reason "TEXCOORD_0" becomes "TEXCOORD_0sssss"
				if (texIt == prim.attributes.end())
				{
					for (auto it = prim.attributes.begin(); it != prim.attributes.end(); it++)
					{
						if (it->first.starts_with("TEXCOORD_0"))
						{
							texIt = it;
							break;
						}
					}
				}

				// populate the vertex attributes
				subset.Vertices.resize(model.accessors[posIt->second].count);

				//const uint64_t meshID = MeshID(sceneID, subset.MeshIdx, subset.MeshPrimIdx);

				// POSITION
				ProcessPositions(model, posIt->second, subset.Vertices);

				// NORMAL
				ProcessNormals(model, normalit->second, subset.Vertices);

				// TEXCOORD_0
				if (texIt != prim.attributes.end())
					ProcessTexCoords(model, texIt->second, subset.Vertices);

				// indices
				ProcessIndices(model, prim.indices, subset.Indices);

				// TANGENT
				auto tangentIt = prim.attributes.find("TANGENT");

				// if vertex tangents aren't present, compute them. Make sure the computation happens after 
				// vertex & index processing
				if (tangentIt != prim.attributes.end())
					ProcessTangents(model, tangentIt->second, subset.Vertices);
				else
					Math::ComputeMeshTangentVectors(subset.Vertices, subset.Indices, false);

				subset.MaterialIdx = prim.material;
				scene.AddMesh(sceneID, ZetaMove(subset));

				primIdx++;
			}
		}
	}

	void ProcessMaterials(uint64_t sceneID, const Filesystem::Path& modelDir, const tinygltf::Model& model,
		int offset, int size) noexcept
	{
		SceneCore& scene = App::GetScene();

		auto getAlphaMode = [](const std::string& s) noexcept
		{
			auto ret = Material::ALPHA_MODE::OPAQUE_;

			if (strcmp(s.data(), "MASK") == 0)
				ret = Material::ALPHA_MODE::MASK;
			else if (strcmp(s.data(), "BLEND") == 0)
				ret = Material::ALPHA_MODE::BLEND;

			return ret;
		};

		for (int m = offset; m != offset + size; m++)
		{
			const auto& mat = model.materials[m];

			glTF::Asset::MaterialDesc desc;
			desc.Index = m;
			desc.AlphaMode = getAlphaMode(mat.alphaMode);
			desc.AlphaCuttoff = (float)mat.alphaCutoff;
			desc.DoubleSided = mat.doubleSided;

			// base color map
			{
				const int baseColIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
				if (baseColIdx != -1)
				{
					const int imgIdx = model.textures[baseColIdx].source;
					Check(imgIdx != -1, "Invalid texture index");
					const std::string& texPath = model.images[imgIdx].uri;

					desc.BaseColorTexPath.Reset(App::GetAssetDir());
					desc.BaseColorTexPath.Append(modelDir.Get());
					desc.BaseColorTexPath.Append(texPath.data());
				}

				auto& f = mat.pbrMetallicRoughness.baseColorFactor;
				Check(f.size() == 4, "Invalid BaseColorFactor");
				desc.BaseColorFactor = float4((float)f[0], (float)f[1], (float)f[2], (float)f[3]);
			}

			// normal map
			{
				const int normalTexIdx = mat.normalTexture.index;
				if (normalTexIdx != -1)
				{
					const int imgIdx = model.textures[normalTexIdx].source;
					Check(imgIdx != -1, "Invalid texture index");
					const std::string& texPath = model.images[imgIdx].uri;

					desc.NormalTexPath.Reset(App::GetAssetDir());
					desc.NormalTexPath.Append(modelDir.Get());
					desc.NormalTexPath.Append(texPath.data());
				}

				desc.NormalScale = (float)mat.normalTexture.scale;
			}

			// metalness-roughness map
			{
				const int metalnessRoughnessIdx = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
				if (metalnessRoughnessIdx != -1)
				{
					const int imgIdx = model.textures[metalnessRoughnessIdx].source;
					Check(imgIdx != -1, "Invalid texture index");
					const std::string& texPath = model.images[imgIdx].uri;

					desc.MetalnessRoughnessTexPath.Reset(App::GetAssetDir());
					desc.MetalnessRoughnessTexPath.Append(modelDir.Get());
					desc.MetalnessRoughnessTexPath.Append(texPath.data());
				}

				desc.MetalnessFactor = (float)mat.pbrMetallicRoughness.metallicFactor;
				desc.RoughnessFactor = (float)mat.pbrMetallicRoughness.roughnessFactor;
			}

			// emissive map
			{
				const int emissiveIdx = mat.emissiveTexture.index;
				if (emissiveIdx != -1)
				{
					const int imgIdx = model.textures[emissiveIdx].source;
					Check(imgIdx != -1, "Invalid texture index");
					const std::string& texPath = model.images[imgIdx].uri;

					desc.EmissiveTexPath.Reset(App::GetAssetDir());
					desc.EmissiveTexPath.Append(modelDir.Get());
					desc.EmissiveTexPath.Append(texPath.data());
				}

				auto& f = mat.emissiveFactor;
				Check(f.size() == 3, "Invalid emissiveFactor");
				desc.EmissiveFactor = float3((float)f[0], (float)f[1], (float)f[2]);
			}

			scene.AddMaterial(sceneID, ZetaMove(desc));
		}
	}

	void ProcessNodeSubtree(const tinygltf::Node& node, uint64_t sceneID, const tinygltf::Model& model, uint64_t parentId,
		Vector<IntemediateInstance, App::ThreadAllocator>& instances) noexcept
	{
		uint64_t currInstanceID = SceneCore::ROOT_ID;

		if (node.mesh != -1)
		{
			AffineTransformation transform = AffineTransformation::GetIdentity();

			if (node.matrix.size() == 16)
			{
				float4x4a M(node.matrix.data());
				v_float4x4 vM = load(M);
				auto det = store(det3x3(vM));	// last column is ignored
				Check(fabsf(det.x) > 1e-6f, "Transformation matrix with a zero determinant is invalid.");
				Check(det.x > 0.0f, "Transformation matrices that change the orientation (e.g. negative scaling) are not supported.");

				// RHS transformation matrix M_rhs can be converted to LHS (+Y up) as follows:
				//
				//		transform = M_RhsToLhs * M_rhs * M_LhsToRhs
				// 
				// where M_RhsToLhs is a change-of-basis transformation matrix and M_LhsToRhs = M_RhsToLhs^-1. 
				// Replacing in above:
				//
				//                  | 1 0  0 |             | 1 0  0 |
				//		transform = | 0 1  0 | * [u v w] * | 0 1  0 |
				//                  | 0 0 -1 |             | 0 0 -1 |
				//
				//                  | 1 0  0 |                  
				//                = | 0 1  0 | * [u v -w]
				//                  | 0 0 -1 |
				//
				//                  |  u_1  v_1  -w_1 |                  
				//                = |  u_2  v_2  -w_2 |
				//                  | -u_3 -v_3   w_3 |
				M.m[0].z *= -1.0f;
				M.m[1].z *= -1.0f;
				M.m[2].x *= -1.0f;
				M.m[2].y *= -1.0f;

				// convert translation to LHS (translation is not a linear transformation, so the approach above
				// doesn't work)
				M.m[2].w *= -1.0f;

				decomposeTRS(vM, transform.Scale, transform.Rotation, transform.Translation);
			}
			else
			{
				if (!node.scale.empty())
				{
					Check(node.scale[0] > 0 && node.scale[1] > 0 && node.scale[2] > 0, "Negative or zero scale factors are not supported.");
					transform.Scale = float3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);
				}

				if (!node.translation.empty())
					transform.Translation = float3((float)node.translation[0], (float)node.translation[1], (float)-node.translation[2]);

				if (!node.rotation.empty())
				{
					// rotation quaternion = (n_x * s, n_y * s, n_z * s, c)
					// where s = sin(theta/2) and c = cos(theta/2)
					//
					// In the left-handed coord. system here with +Y as up, n_lhs = (n_x, n_y, -n_z)
					// and theta_lhs = -theta. Since sin(-a) = -sin(a) and cos(-a) = cos(a) we have:
					//
					//		q_lhs = (n_x * -s, n_y * -s, -n_z * -s, c)
					//			  = (-n_x * s, -n_y * s, n_z * s, c)
					//
					//float4a q = float4a(-(float)node.rotation[0], -(float)node.rotation[1],
					//	(float)node.rotation[2], (float)node.rotation[3]);
					transform.Rotation = float4(-(float)node.rotation[0], 
						-(float)node.rotation[1],
						(float)node.rotation[2], 
						(float)node.rotation[3]);

					// check if quaternion is a valid rotation
					__m128 vV = _mm_loadu_ps(&transform.Rotation.x);
					__m128 vLength = _mm_dp_ps(vV, vV, 0xff);
					vLength = _mm_sqrt_ps(vLength);
					__m128 vOne = _mm_set1_ps(1.0f);
					__m128 vDiff = _mm_sub_ps(vLength, vOne);
					float d = _mm_cvtss_f32(abs(vDiff));
					Check(d < 1e-6f, "Invalid rotation quaternion.");
				}
			}

			instances.emplace_back(IntemediateInstance{
						.LocalTransform = transform,
						.MeshIdx = node.mesh,
						.Name = node.name,
						.ParentID = parentId
				});

			// each mesh has at least one primitive and any of those can be designated as the parent instance
			// note Scene calucaltes instance ID with proper meshPrimitive index, this instance ID is only 
			// used to establish parent-child relationships
			currInstanceID = SceneCore::InstanceID(sceneID, node.name.c_str(), node.mesh, 0);
		}

		for (int c : node.children)
		{
			const tinygltf::Node& childNode = model.nodes[c];
			ProcessNodeSubtree(childNode, sceneID, model, currInstanceID, instances);
		}
	}

	void ProcessNodes(const tinygltf::Model& model, uint64_t sceneID, Vector<IntemediateInstance, App::ThreadAllocator>& instances) noexcept
	{
		for (int i : model.scenes[model.defaultScene].nodes)
		{
			const tinygltf::Node& node = model.nodes[i];
			ProcessNodeSubtree(node, sceneID, model, SceneCore::ROOT_ID, instances);
		}
	}

	void ProcessInstances(uint64_t sceneID, Span<IntemediateInstance> instances, const tinygltf::Model& model) noexcept
	{
		for (auto& instance : instances)
		{
			const int meshIdx = instance.MeshIdx;
			auto& mesh = model.meshes[meshIdx];
			int meshPrimIdx = 0;

			for (auto& meshPrim : model.meshes[meshIdx].primitives)
			{
				Check(meshPrim.material != -1, "Following mesh doesn't have any materials assigned to it: %s (#primitive %d)",
					instance.Name.data(), meshIdx);

				uint8_t rtInsMask = model.materials[meshPrim.material].emissiveTexture.index != -1 ?
					RT_AS_SUBGROUP::EMISSIVE : RT_AS_SUBGROUP::NON_EMISSIVE;

				glTF::Asset::InstanceDesc desc{
					.LocalTransform = instance.LocalTransform,
					.MeshIdx = meshIdx,
					.Name = instance.Name.data(),
					.ParentID = instance.ParentID,
					.MeshPrimIdx = meshPrimIdx,
					.RtMeshMode = RT_MESH_MODE::STATIC,
					.RtInstanceMask = rtInsMask };

				/*
				if (rtInsMask & RT_AS_SUBGROUP::EMISSIVE)
				{
					uint64_t ID = MeshID(sceneID, meshIdx, meshPrimIdx);

					int idx = FindMeshPrim(emissives, ID);
					Check(idx != -1, "Invalid emissive");
					desc.Lumen.swap(emissives[idx].Lumens);
				}
				*/

				SceneCore& scene = App::GetScene();
				scene.AddInstance(sceneID, ZetaMove(desc));

				meshPrimIdx++;
			}
		}
	}
}

// TODO change the relative path
void glTF::Load(const char* modelRelPath) noexcept
{
	tinygltf::TinyGLTF loader;
	tinygltf::Model model;
	std::string error;
	std::string warning;

	Filesystem::Path fullPath(App::GetAssetDir());
	fullPath.Append(modelRelPath);
	std::string s(fullPath.Get());
	const bool success = loader.LoadASCIIFromFile(&model, &error, &warning, s);

	Check(warning.empty(), "Warning while loading glTF2 model from path %s: %s.", s.c_str(), warning.c_str());
	Check(error.empty(), "Error while loading glTF2 model from path %s: %s.", s.c_str(), error.c_str());
	Check(model.defaultScene != -1, "invalid defaultScene value.");

	const uint64_t sceneID = XXH3_64bits(s.c_str(), s.size());
	SceneCore& scene = App::GetScene();

	size_t numMeshes = 0;
	for (const auto& mesh : model.meshes)
		numMeshes += mesh.primitives.size();

	scene.ReserveScene(sceneID, numMeshes, model.materials.size(), model.nodes.size());

	// how many meshes are processed by each worker
	constexpr size_t MAX_NUM_MESH_WORKERS = 3;
	constexpr size_t MIN_MESHES_PER_WORKER = 20;
	size_t meshThreadOffsets[MAX_NUM_MESH_WORKERS];
	size_t meshThreadSizes[MAX_NUM_MESH_WORKERS];

	const size_t meshNumThreads = SubdivideRangeWithMin(model.meshes.size(),
		MAX_NUM_MESH_WORKERS,
		meshThreadOffsets,
		meshThreadSizes,
		MIN_MESHES_PER_WORKER);

	// how many materials are processed by each worker
	constexpr size_t MAX_NUM_MAT_WORKERS = 3;
	constexpr size_t MIN_MATS_PER_WORKER = 20;
	size_t matThreadOffsets[MAX_NUM_MAT_WORKERS];
	size_t matThreadSizes[MAX_NUM_MAT_WORKERS];

	const size_t matNumThreads = SubdivideRangeWithMin(model.materials.size(),
		MAX_NUM_MAT_WORKERS,
		matThreadOffsets,
		matThreadSizes,
		MIN_MATS_PER_WORKER);

	struct ThreadContext
	{
		uint64_t SceneID;
		tinygltf::Model* Model;
		size_t* MeshThreadOffsets;
		size_t* MeshThreadSizes;
		size_t* MatThreadOffsets;
		size_t* MatThreadSizes;
	};

	ThreadContext tc{ .SceneID = sceneID, .Model = &model,
		.MeshThreadOffsets = meshThreadOffsets, .MeshThreadSizes = meshThreadSizes,
		.MatThreadOffsets = matThreadOffsets, .MatThreadSizes = matThreadSizes };

	TaskSet ts;

	for (size_t i = 0; i < meshNumThreads; i++)
	{
		StackStr(tname, n, "gltf::ProcessMesh_%d", i);

		ts.EmplaceTask(tname, [&tc, rangeIdx = i]()
			{
				ProcessMeshes(tc.SceneID, *tc.Model, tc.MeshThreadOffsets[rangeIdx], tc.MeshThreadSizes[rangeIdx]);
			});
	}

	for (size_t i = 0; i < matNumThreads; i++)
	{
		StackStr(tname, n, "gltf::ProcessMats_%d", i);

		ts.EmplaceTask(tname, [&modelRelPath, &tc, rangeIdx = i]()
			{
				Filesystem::Path parent(modelRelPath);
				parent.ToParent();

				ProcessMaterials(tc.SceneID, parent, *tc.Model, (int)tc.MatThreadOffsets[rangeIdx], (int)tc.MatThreadSizes[rangeIdx]);
			});
	}

	WaitObject waitObj;
	ts.Sort();
	ts.Finalize(&waitObj);
	App::Submit(ZetaMove(ts));

	SmallVector<IntemediateInstance, App::ThreadAllocator> instances;
	instances.reserve(model.nodes.size());

	// TODO is this necessary?
	waitObj.Wait();

	ProcessNodes(model, sceneID, instances);
	ProcessInstances(sceneID, instances, model);
}