#pragma once

#include "../Core/Vertex.h"
#include "../Core/GpuMemory.h"
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
			: BaseColorTexPath(uint64_t(-1)),
			MetalnessRoughnessTexPath(uint64_t(-1)),
			NormalTexPath(uint64_t(-1)),
			EmissiveTexPath(uint64_t(-1)),
			MetalnessFactor(1.0f),
			RoughnessFactor(1.0f),
			NormalScale(1.0f),
			AlphaCuttoff(0.5f),
			AlphaMode(Material::ALPHA_MODE::OPAQUE),
			DoubleSided(false),
			Index(-1),
			BaseColorFactor(1.0f, 1.0f, 1.0f, 1.0f),
			EmissiveFactor(0.0f, 0.0f, 0.0f)
		{
		}

		// Unique index of each material within the glTF scene
		int Index;

		uint64_t BaseColorTexPath;
		uint64_t MetalnessRoughnessTexPath;
		uint64_t NormalTexPath;
		uint64_t EmissiveTexPath;

		Math::float4 BaseColorFactor;
		Math::float3 EmissiveFactor;
		float MetalnessFactor;
		float RoughnessFactor;
		float NormalScale;

		float AlphaCuttoff;
		Material::ALPHA_MODE AlphaMode;
		bool DoubleSided;
	};

	struct alignas(64) DDSImage
	{
		Core::Texture T;
		uint64_t ID;
	};
}