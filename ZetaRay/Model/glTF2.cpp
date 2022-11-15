#include "glTF2.h"
#include "../Core/Vertex.h"
#include "../Math/MatrixFuncs.h"
#include "../Math/Surface.h"
#include "../Math/Quaternion.h"
#include "../Scene/Assets.h"
#include "../Scene/SceneCore.h"
#include "Mesh.h"
#include "../RenderPass/Common/RtCommon.h"
#include "../Support/Task.h"
#include "../Win32/Filesystem.h"
#include "../Win32/Log.h"
#include "../Win32/Timer.h"

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
using namespace ZetaRay::Model::glTF2;
using namespace ZetaRay::Win32;

//--------------------------------------------------------------------------------------
// gltfModel
//--------------------------------------------------------------------------------------

namespace
{
	inline uint64_t MeshID(uint64_t sceneID, int meshIdx, int meshPrimIdx) noexcept
	{
		StackStr(str, n, "mesh_%llu_%d_%d", sceneID, meshIdx, meshPrimIdx);
		uint64_t meshFromSceneID = XXH3_64bits(str, n);

		return meshFromSceneID;
	}

	struct IntemediateInstance
	{
		float4x3 LocalTransform;
		int MeshIdx;
		std::string_view Name;
		uint64_t ParentID;
	};

	/*
	struct EmissiveMeshPower
	{
		EmissiveMeshPower(uint64_t sceneID, int meshIdx, int primIdx, int numTris)
		{
			ID = MeshID(sceneID, meshIdx, primIdx);
			Lumens.resize(numTris);
		}

		uint64_t ID;
		SmallVector<float, 2, 32> Lumens;
	};

	// Search the range [beg, end) for key "key"
	int FindMeshPrim(const Vector<EmissiveMeshPower>& emissives, uint64_t key)
	{
		int beg = 0;
		int end = (int)emissives.size();
		int mid = (int)(emissives.size() >> 1);

		if (beg == end)
			return -1;

		while (true)
		{
			if (end - beg <= 2)
				break;

			if (emissives[mid].ID < key)
				beg = mid + 1;
			else
				end = mid + 1;

			mid = beg + ((end - beg) >> 1);
		}

		if (emissives[beg].ID == key)
			return beg;
		else if (emissives[mid].ID == key)
			return mid;

		return -1;
	}
	*/

	void ProcessPositions(const tinygltf::Model& model, int posIdx, Vector<VertexPosNormalTexTangent>& vertices) noexcept
	{
		const auto& accessor = model.accessors[posIdx];

		Check(accessor.type == TINYGLTF_TYPE_VEC3, "Invalid type for POSITION attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for POSITION attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride == sizeof(float3), "Invalid stride for POSITION attribute.");

		// populate the vertex position attribute
		const auto& buffer = model.buffers[bufferView.buffer];
		float3* start = (float3*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			float3* curr = start + i;

			// glTF uses a right-handed coordinate systes with +Y as up
			vertices[i].Position = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessNormals(const tinygltf::Model& model, int normalIdx, Vector<VertexPosNormalTexTangent>& vertices) noexcept
	{
		const auto& accessor = model.accessors[normalIdx];

		Check(accessor.type == TINYGLTF_TYPE_VEC3, "Invalid type for NORMAL attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for NORMAL attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride == sizeof(float3), "Invalid stride for NORMAL attribute.");

		const auto& buffer = model.buffers[bufferView.buffer];
		float3* start = (float3*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			float3* curr = start + i;

			// glTF uses a right-handed coordinate systes with +Y as up
			vertices[i].Normal = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessTexCoords(const tinygltf::Model& model, int uv0Idx, Vector<VertexPosNormalTexTangent>& vertices) noexcept
	{
		const auto& accessor = model.accessors[uv0Idx];

		Check(accessor.type == TINYGLTF_TYPE_VEC2, "Invalid type for TEXCOORD_0 attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT, 
			"Invalid component type for TEXCOORD_0 attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride == sizeof(float2), "Invalid stride for TEXCOORD_0 attribute.");

		// populate the vertex TexUV attribute
		const auto& buffer = model.buffers[bufferView.buffer];
		float2* start = (float2*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			float2* curr = start + i;
			vertices[i].TexUV = float2(curr->x, curr->y);
		}
	}

	void ProcessTangents(const tinygltf::Model& model, int tangentIdx, Vector<VertexPosNormalTexTangent>& vertices) noexcept
	{
		const auto& accessor = model.accessors[tangentIdx];

		Check(accessor.type == TINYGLTF_TYPE_VEC3 || accessor.type == TINYGLTF_TYPE_VEC4, "Invalid type for TANGENT attribute.");
		Check(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
			"Invalid component type for TANGENT attribute.");

		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);

		const auto& buffer = model.buffers[bufferView.buffer];
		float4* start = (float4*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			float4* curr = start + i;

			// glTF uses a right-handed coordinate systes with +Y as up
			vertices[i].Tangent = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessIndices(const tinygltf::Model& model, int indicesIdx, Vector<INDEX_TYPE>& indices) noexcept
	{
		const auto& accessor = model.accessors[indicesIdx];

		Check(accessor.type == TINYGLTF_TYPE_SCALAR, "Invalid index type.");
		//Check((accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
		//	accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT), 
		//	"Index component type not supported.");
		bool indexType32Bit = accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;

		auto& bufferView = model.bufferViews[accessor.bufferView];
		const int byteStride = accessor.ByteStride(bufferView);
		Check(byteStride != -1, "Invalid index stride.");

		auto& buffer = const_cast<tinygltf::Model&>(model).buffers[bufferView.buffer];
		indices.reserve(accessor.count);

		// populate the mesh indices
		uint8_t* start = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
		uint8_t* curr = start;
		Assert(accessor.count % 3 == 0, "invalid number of indices");
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

#if USE_16_BIT_INDICES
			Assert(!indexType32Bit || (i0 < UINT16_MAX && i1 < UINT16_MAX && i2 < UINT16_MAX), "32-bit indices are not supported");
#endif // INDEX_TYPE == uint16_t

			// changing the handedness to left handed was not needed, why?

			indices.push_back(static_cast<INDEX_TYPE>(i0));
			indices.push_back(static_cast<INDEX_TYPE>(i2));
			indices.push_back(static_cast<INDEX_TYPE>(i1));
		}

		Assert(indices.size() == numFaces * 3, "bug");
	}

	void ProcessMeshes(uint64_t sceneID, tinygltf::Model& model, size_t offset, size_t size) noexcept
	{
		SceneCore& scene = App::GetScene();

		for (size_t meshIdx = offset; meshIdx != offset + size; meshIdx++)
		{
			Assert(meshIdx < model.meshes.size(), "bug");
			const auto& mesh = model.meshes[meshIdx];
			int primIdx = 0;

			// fill in the subsets
			for (const auto& prim : mesh.primitives)
			{
				Asset::MeshSubset subset;
				subset.MeshIdx = (int)meshIdx;
				subset.MeshPrimIdx = primIdx;

				Check(prim.indices != -1, "No Index buffer was set.");
				Check(prim.mode == TINYGLTF_MODE_TRIANGLES, "Non-triangle meshes are not supported.");

				auto posIt = prim.attributes.find("POSITION");

				// workaround for weird bug when /fsanitize=address is used. For some
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
				
				//Check((posIit != prim.attributes.end() || posIit->second != -1),
				//	"POSITION was not found in the vertex attributes.");
				Check(posIt != prim.attributes.end(), "POSITION was not found in the vertex attributes.");

				auto normalit = prim.attributes.find("NORMAL");
				//Check((normalit != prim.attributes.end() || normalit->second != -1),
				//	"NORMAL was not found in the vertex attributes.");
				Check(normalit != prim.attributes.end(), "NORMAL was not found in the vertex attributes.");

				auto texIt = prim.attributes.find("TEXCOORD_0");

				// workaround for weird bug when /fsanitize=address is used. For some
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

				uint64_t meshID = MeshID(sceneID, subset.MeshIdx, subset.MeshPrimIdx);

				// POSITION
				ProcessPositions(model, posIt->second, subset.Vertices);

				// NORMAL
				ProcessNormals(model, normalit->second, subset.Vertices);

				// TEXCOORD_0
				if(texIt != prim.attributes.end())
					ProcessTexCoords(model, texIt->second, subset.Vertices);

				// index buffer
				ProcessIndices(model, prim.indices, subset.Indices);

				// TANGENT
				auto tangentIt = prim.attributes.find("TANGENT");

				// if vertex tangent's aren't present, compute them. Make sure the computation happens after 
				// vertex & index processing
				if (tangentIt != prim.attributes.end())
					ProcessTangents(model, tangentIt->second, subset.Vertices);
				else
				{
					bool s = Math::ComputeMeshTangentVectors(subset.Vertices, subset.Indices, true);
					//Check(s, "Failed to compute vertex tangent vectors");

					if (!s)
					{
						printf("Failed to compute vertex tangent vectors for mesh %llu, primitive %d\n", meshIdx, primIdx);

						for (auto& v : subset.Vertices)
							v.Tangent = float3(0.0f, 0.0f, 0.0f);
					}
				}
		
				subset.MaterialIdx = prim.material;
				scene.AddMesh(sceneID, ZetaMove(subset));

				primIdx++;
			}
		}
	}

	void ProcessMaterials(uint64_t sceneID, const Filesystem::Path& modelDir, tinygltf::Model& model, 
		size_t offset, size_t size) noexcept
	{
		SceneCore& scene = App::GetScene();

		auto getAlphaMode = [](const std::string& s) noexcept
		{
			auto ret = Material::ALPHA_MODE::OPAQUE;

			if (strcmp(s.data(), "MASK") == 0)
				ret =  Material::ALPHA_MODE::MASK;
			else if (strcmp(s.data(), "BLEND") == 0)
				ret =  Material::ALPHA_MODE::BLEND;

			return ret;
		};

		for (size_t m = offset; m != offset + size; m++) 
		{
			const auto& mat = model.materials[m];
			Asset::MaterialDesc desc;
			desc.Index = (uint32_t)m;

			desc.AlphaMode = getAlphaMode(mat.alphaMode);
			desc.AlphaCuttoff = max(MIN_ALPHA_CUTOFF, (float)mat.alphaCutoff);
			desc.TwoSided = mat.doubleSided;

			{
				int baseColIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
				if (baseColIdx != -1)
				{
					int imgIdx = model.textures[baseColIdx].source;
					Check(imgIdx != -1, "Invalid image-index");
					std::string& texPath = model.images[imgIdx].uri;

					desc.BaseColorTexPath = Filesystem::Path(App::GetAssetDir());
					desc.BaseColorTexPath.Append(modelDir.Get());
					desc.BaseColorTexPath.Append(texPath.data());
				}

				auto& f = mat.pbrMetallicRoughness.baseColorFactor;
				Check(f.size() == 4, "Invalid BaseColorFactor");
				desc.BaseColorFactor = float4((float)f[0], (float)f[1], (float)f[2], (float)f[3]);
			}

			{
				int normalTexIdx = mat.normalTexture.index;
				if (normalTexIdx != -1)
				{
					int imgIdx = model.textures[normalTexIdx].source;
					Check(imgIdx != -1, "Invalid image-index");
					std::string& texPath = model.images[imgIdx].uri;

					desc.NormalTexPath = Filesystem::Path(App::GetAssetDir());
					desc.NormalTexPath.Append(modelDir.Get());
					desc.NormalTexPath.Append(texPath.data());
				}

				desc.NormalScale = (float)mat.normalTexture.scale;
			}

			{
				int metalicRoughnessIdx = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
				if (metalicRoughnessIdx != -1)
				{
					int imgIdx = model.textures[metalicRoughnessIdx].source;
					Check(imgIdx != -1, "Invalid image-index");
					std::string& texPath = model.images[imgIdx].uri;

					desc.MetalnessRoughnessTexPath = Filesystem::Path(App::GetAssetDir());
					desc.MetalnessRoughnessTexPath.Append(modelDir.Get());
					desc.MetalnessRoughnessTexPath.Append(texPath.data());
				}

				desc.MetallicFactor = (float)mat.pbrMetallicRoughness.metallicFactor;
				desc.RoughnessFactor = (float)mat.pbrMetallicRoughness.roughnessFactor;
			}

			{
				int emissiveIdx = mat.emissiveTexture.index;
				if (emissiveIdx != -1)
				{
					int imgIdx = model.textures[emissiveIdx].source;
					Check(imgIdx != -1, "Invalid image-index");
					std::string& texPath = model.images[imgIdx].uri;

					desc.EmissiveTexPath = Filesystem::Path(App::GetAssetDir());
					desc.EmissiveTexPath.Append(modelDir.Get());
					desc.EmissiveTexPath.Append(texPath.data());
				}

				auto& f = mat.emissiveFactor;
				Assert(f.size() == 3, "Invalid emissiveFactor");
				desc.EmissiveFactor = float3((float)f[0], (float)f[1], (float)f[2]);
			}
				
			scene.AddMaterial(sceneID, ZetaMove(desc));
		}
	}

	void ProcessNodeSubtree(const tinygltf::Node& node, uint64_t sceneID, const tinygltf::Model& model, uint64_t parentId, 
		Vector<IntemediateInstance>& instances, bool blenderToYupConversion) noexcept
	{
		uint64_t currInstanceID = SceneCore::ROOT_ID;

		if (node.mesh != -1)
		{
			// glTF uses a right-handed coordinate systes with +Y as up (source). Here, a left-handed
			// coord. system is used where +X points to right, +Y point up and +Z points inside
			// the screen (target). To convert between those, use a change-of-coordinate transformation. 
			// In "target" coord. system, XYZ basis vectors of "source" are as follows:
			//	X+: (1, 0, 0)
			//	Y+: (0, 1, 0)
			//	X+: (0, 0, -1)
			v_float4x4 vRhsToLhs = scale(1.0f, 1.0f, -1.0f);
			float4x4a M;
			v_float4x4 vM;

			if (node.matrix.size() == 16)
			{
				M = float4x4a(node.matrix.data());

				// tranpose the tranformation matrix to get a "row" matrix
				vM = transpose(v_float4x4(M));
			}
			else
			{
				v_float4x4 vS = identity();
				v_float4x4 vT = identity();
				v_float4x4 vR = identity();

				if (!node.scale.empty())
					vS = scale((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);

				if (!node.translation.empty())
					vT = translate(float4a((float)node.translation[0], 
						(float)node.translation[1],
						(float)-node.translation[2], 0.0f));

				if (!node.rotation.empty())
				{
					float4a q = float4a((float)node.rotation[0], (float)node.rotation[1],
						(float)node.rotation[2], (float)node.rotation[3]);

					__m128 vQ = _mm_load_ps(reinterpret_cast<float*>(&q));
					vR = rotationMatrixFromQuat(vQ);
				}

				if (blenderToYupConversion)
				{
					//v_float4x4 vRotAboutX = rotateX(Math::PI_DIV_2);
					v_float4x4 vRotAboutX = rotateX(Math::PI);
					vR = mul(vR, vRotAboutX);
				}

				//v_float4x4 vRotAboutX = rotateX(Math::PI_DIV_2);
				//vR = mul(vR, vRotAboutX);
				//vR = mul(vR, vRhsToLhs);
				vM = mul(vS, vR);
				vM = mul(vM, vT);
			}

			//vM = mul(vM, vRhsToLhs);
			M = store(vM);

			instances.emplace_back(IntemediateInstance{
						.LocalTransform = float4x3(M),
						.MeshIdx = node.mesh,
						.Name = node.name,
						.ParentID = parentId
				});

			// each mesh has at least 1 primitive and any of those can be designated as the parent instance
			// note Scene calucaltes instance ID with proper meshPrimitive index, this instance ID is only 
			// used to establish parent-child relationships
			currInstanceID = SceneCore::InstanceID(sceneID, node.name.c_str(), node.mesh, 0);
		}

		for (int c : node.children)
		{
			const tinygltf::Node& node = model.nodes[c];
			ProcessNodeSubtree(node, sceneID, model, currInstanceID, instances, blenderToYupConversion);
		}
	}

	void ProcessNodes(const tinygltf::Model& model, uint64_t sceneID, Vector<IntemediateInstance>& instances, bool blenderToYupConversion) noexcept
	{
		for (int i : model.scenes[model.defaultScene].nodes)
		{
			const tinygltf::Node& node = model.nodes[i];
			ProcessNodeSubtree(node, sceneID, model, SceneCore::ROOT_ID, instances, blenderToYupConversion);
		}
	}

	void ProcessInstances(uint64_t sceneID, const Vector<IntemediateInstance>& instances, const tinygltf::Model& model) noexcept
	{
		for (auto& instance : instances)
		{
			const int meshIdx = instance.MeshIdx;
			auto& mesh = model.meshes[meshIdx];
			int meshPrimIdx = 0;

			for (auto& meshPrim : model.meshes[meshIdx].primitives)
			{
				Check(meshPrim.material != -1, "Mesh doesn't have any materials assigned to it.");

				uint8_t rtInsMask = model.materials[meshPrim.material].emissiveTexture.index != -1 ? 
					RT_AS_SUBGROUP::EMISSIVE : RT_AS_SUBGROUP::NON_EMISSIVE;

				Asset::InstanceDesc desc{ 
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

	/*
	void ProcessEmissives(std::string_view path, uint64_t sceneID, Vector<EmissiveMeshPower>& emissives) noexcept
	{
		if(!Win32::Filesystem::Exists(path.data()))
			return;

		// load the file
		HANDLE h = CreateFileA(path.data(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);

		CheckWin32(h);

		LARGE_INTEGER s;
		CheckWin32(GetFileSizeEx(h, &s));

		uint8_t* buff = (uint8_t*)_aligned_malloc(s.QuadPart, 32);
		DWORD numRead;
		CheckWin32(ReadFile(h, buff, (DWORD)s.QuadPart, &numRead, nullptr));
		Check(numRead == (DWORD)s.QuadPart, "ReadFile() read %u bytes, requested size: %u", numRead, (DWORD)s.QuadPart);

		CloseHandle(h);

		// header
		const char header[] = "glTF2Emissives;";
		Check(strcmp((char*)buff, header) == 0, "Invalid header");

		uint8_t* ptr = buff + sizeof(header) + 1;
		int meshIdx;
		int primIdx;
		int numTriangles;

		while ((uintptr_t)ptr - (uintptr_t)buff < (uintptr_t)s.QuadPart)
		{
			memcpy(&meshIdx, ptr, sizeof(int));
			ptr += sizeof(int);

			memcpy(&primIdx, ptr, sizeof(int));
			ptr += sizeof(int);

			memcpy(&numTriangles, ptr, sizeof(int));
			ptr += sizeof(int);

			Check(meshIdx >= 0, "Invalid meshidx");
			Check(primIdx >= 0, "Invalid meshidx");
			Check(numTriangles > 0, "Invalid numTriangles");

			EmissiveMeshPower e(sceneID, meshIdx, primIdx, numTriangles);

			const int numToProcSIMD = numTriangles - (numTriangles & (8 - 1));
			ptr = reinterpret_cast<uint8_t*>(((uintptr_t)ptr + 31) & ~31);
			Check(((uintptr_t)ptr & (32 - 1)) == 0, "must be 32-byte aligned.");

			for (int i = 0; i < numToProcSIMD; i += 8)
			{
				__m256 V = _mm256_load_ps(reinterpret_cast<float*>(ptr));
				_mm256_store_ps(&e.Lumens[i], V);

				ptr += 32;
			}

			for (int i = numToProcSIMD; i < numTriangles; i++)
			{
				memcpy(&e.Lumens[i], ptr, sizeof(float));
				ptr += sizeof(float);
			}

			Check(*ptr++ == ';', "Unexpected character.");

			emissives.push_back(e);
		}

		_aligned_free(buff);
	}
	*/
}

void Model::glTF2::Load(const char* modelRelPath, bool blenderToYupConversion) noexcept
{
	tinygltf::TinyGLTF loader;
	tinygltf::Model model;
	std::string error;
	std::string warning;

	Filesystem::Path fullPath(App::GetAssetDir());
	fullPath.Append(modelRelPath);
	std::string s(fullPath.Get());
	const bool success = loader.LoadASCIIFromFile(&model, &error, &warning, s);

	Check(warning.empty(), "Warning while loading glTF2 model from %s: %s.", s.c_str(), warning.c_str());
	Check(error.empty(), "Error while loading glTF2 model from %s: %s.", s.c_str(), error.c_str());
	Check(model.defaultScene != -1, "invalid defaultScene value.");

	const uint64_t sceneID = XXH3_64bits(s.c_str(), s.size());
	SceneCore& scene = App::GetScene();

	size_t numMeshes = 0;
	for (const auto& mesh : model.meshes)
		numMeshes += mesh.primitives.size();

	scene.ReserveScene(sceneID, numMeshes, model.materials.size(), model.nodes.size());

	constexpr size_t MAX_NUM_MESH_WORKERS = 3;
	constexpr size_t MIN_MESH_PER_WORKER = 20;
	size_t meshThreadOffsets[MAX_NUM_MESH_WORKERS];
	size_t meshThreadSizes[MAX_NUM_MESH_WORKERS];
	size_t meshNumThreads = SubdivideRangeWithMin(model.meshes.size(), MAX_NUM_MESH_WORKERS, meshThreadOffsets, meshThreadSizes, MIN_MESH_PER_WORKER);

	constexpr size_t MAX_NUM_MAT_WORKERS = 3;
	constexpr size_t MIN_MAT_PER_WORKER = 20;
	size_t matThreadOffsets[MAX_NUM_MAT_WORKERS];
	size_t matThreadSizes[MAX_NUM_MAT_WORKERS];
	size_t matNumThreads = SubdivideRangeWithMin(model.materials.size(), MAX_NUM_MAT_WORKERS, matThreadOffsets, matThreadSizes, MIN_MAT_PER_WORKER);

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

				// TODO per-texture samplers
				ProcessMaterials(tc.SceneID, parent, *tc.Model, tc.MatThreadOffsets[rangeIdx], tc.MatThreadSizes[rangeIdx]);
			});
	}

	WaitObject waitObj;
	ts.Sort();
	ts.Finalize(&waitObj);
	App::Submit(ZetaMove(ts));

	Win32::DeltaTimer timer;

//	SmallVector<EmissiveMeshPower> emissives;
//	Filesystem::Path emissivePowerPath = fullPath.ToParent().Append("LightSourcePower.bin");

	/*
	timer.Start();
	ProcessEmissives(emissivePowerPath.Get(), sceneID, emissives);
	if (!emissives.empty())
		std::sort(emissives.begin(), emissives.end(), [](EmissiveMeshPower& lhs, EmissiveMeshPower& rhs) {return lhs.ID < rhs.ID; });
	timer.End();
	LOG("Thread %u finished \t%s in %u[us]\n", GetCurrentThreadId(), "ProcessEmissives()", (uint32_t)timer.DeltaMicro());
	*/

	SmallVector<IntemediateInstance> instances;
	instances.reserve(model.nodes.size());

	waitObj.Wait();

	timer.Start();
	ProcessNodes(model, sceneID, instances, blenderToYupConversion);
	ProcessInstances(sceneID, instances, model);
	timer.End();

//	LOG("Thread %u finished \t%s in %u[us]\n", GetCurrentThreadId(), "ProcessInstances()", (uint32_t)timer.DeltaMicro());
}