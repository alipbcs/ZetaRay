#pragma once

#include "../Core/Vertex.h"
#include "../Core/GpuMemory.h"
#include "../Math/Matrix.h"
#include "../App/Filesystem.h"
#include "../Core/Material.h"
#include "../Model/Mesh.h"
#include "../Support/ThreadSafeMemoryArena.h"

namespace ZetaRay::Model::glTF::Asset
{
	struct MeshSubset
	{
		int MaterialIdx;
		int MeshIdx;
		int MeshPrimIdx;
		uint32_t BaseVtxOffset;
		uint32_t BaseIdxOffset;
		uint32_t NumVertices;
		uint32_t NumIndices;
	};

	struct InstanceDesc
	{
		Math::AffineTransformation LocalTransform;
		int MeshIdx;
		uint64_t ID;
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
			AlphaMode(Material::ALPHA_MODE::OPAQUE_),
			DoubleSided(false),
			Index(-1),
			BaseColorFactor(1.0f, 1.0f, 1.0f, 1.0f),
			EmissiveFactor(0.0f, 0.0f, 0.0f),
			EmissiveStrength(1.0f)
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
		float EmissiveStrength;
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