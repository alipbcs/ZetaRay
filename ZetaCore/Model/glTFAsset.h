#pragma once

#include "../Core/Vertex.h"
#include "../Math/Matrix.h"
#include "../App/Filesystem.h"
#include "../Core/Material.h"
#include "../Model/Mesh.h"

namespace ZetaRay::Model::glTF::Asset
{
	struct MeshSubset
	{
		Util::SmallVector<Core::Vertex, App::ThreadAllocator> Vertices;
		Util::SmallVector<uint32_t, App::ThreadAllocator> Indices;
		int MaterialIdx;
		int MeshIdx;
		int MeshPrimIdx;
	};

	struct InstanceDesc
	{
		Math::AffineTransformation LocalTransform;
		int MeshIdx;
		const char* Name;
		uint64_t ParentID;
		int MeshPrimIdx;
		RT_MESH_MODE RtMeshMode;
		uint8_t RtInstanceMask;
	};

	struct MaterialDesc
	{
		MaterialDesc() noexcept
		{
			Index = -1;
			BaseColorFactor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
			EmissiveFactor = Math::float3(0.0f, 0.0f, 0.0f);
			MetalnessFactor = 1.0f;
			RoughnessFactor = 1.0f;
			NormalScale = 1.0f;
			AlphaCuttoff = 0.5f;
			AlphaMode = Material::ALPHA_MODE::OPAQUE;
			DoubleSided = false;
		}

		// Unique index of each material within the glTF scene
		int Index;

		App::Filesystem::Path BaseColorTexPath;
		App::Filesystem::Path MetalnessRoughnessTexPath;
		App::Filesystem::Path NormalTexPath;
		App::Filesystem::Path EmissiveTexPath;

		Math::float4 BaseColorFactor;
		Math::float3 EmissiveFactor;
		float MetalnessFactor;
		float RoughnessFactor;
		float NormalScale;

		float AlphaCuttoff;
		Material::ALPHA_MODE AlphaMode;
		bool DoubleSided;
	};
}