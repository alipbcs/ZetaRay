#pragma once

#include "../Core/Vertex.h"
#include "../Math/Matrix.h"
#include "../App/Filesystem.h"
#include "../RenderPass/Common/Material.h"
#include "../Model/Mesh.h"

namespace ZetaRay::Model::glTF::Asset
{
	struct MeshSubset
	{
		Util::SmallVector<Core::Vertex, App::PoolAllocator> Vertices;
		Util::SmallVector<INDEX_TYPE, App::PoolAllocator> Indices;
		int MaterialIdx;
		int MeshIdx;
		int MeshPrimIdx;
	};

	struct InstanceDesc
	{
		Math::float4x3 LocalTransform;
		int MeshIdx;
		const char* Name;
		uint64_t ParentID;
		int MeshPrimIdx;
		RT_MESH_MODE RtMeshMode;
		uint8_t RtInstanceMask;
	};

	struct MaterialDesc
	{
		void Reset() noexcept
		{
			Index = -1;

			BaseColorFactor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
			EmissiveFactor = Math::float3(0.0f, 0.0f, 0.0f);
			MetalnessFactor = 0.0f;
			RoughnessFactor = 1.0f;
			NormalScale = 1.0f;
			AlphaCuttoff = 0.5f;
			AlphaMode = Material::ALPHA_MODE::OPAQUE;
			TwoSided = false;
		}

		// Unique index of each material within the glTF scene
		int Index;

		// RGB specify the base color of the material (sRGB). fourth component(A) represents the linear
		// alpha coverage of the material
		App::Filesystem::Path BaseColorTexPath;

		// The metallic - roughness texture.The metalness values are sampled from the B channel.The roughness values are sampled
		// from the G channel.These values are linear
		App::Filesystem::Path MetalnessRoughnessTexPath;

		// A tangent space normal map.The texture contains RGB components in linear space. Each texel represents the 
		// XYZ components of a normal vector in tangent space. Red & Green map to [-1 to 1], Blue to Z[1 / 255 to 1]
		App::Filesystem::Path NormalTexPath;
		App::Filesystem::Path EmissiveTexPath;

		// Linear. fourth component (A) is the alpha coverage of the material (interpreted by the alphaMode property). 
		// These values are linear. multiplied baseColorTexture (if present)
		Math::float4 BaseColorFactor;

		// factors for the emissive color of the material
		Math::float3 EmissiveFactor;

		// The metalness of the material (linear) .A value of 1.0 means the material is a metal. A value 
		// of 0.0 means the material is a dielectric
		float MetalnessFactor;

		// The roughness of the material (linear) .A value of 1.0 means the material is completely rough. A 
		// value of 0.0 means the material is completely smooth
		float RoughnessFactor;

		// The scalar multiplier applied to each normal vector of the texture
		float NormalScale;

		// Specifies the cutoff threshold when in MASK mode
		float AlphaCuttoff;
		Material::ALPHA_MODE AlphaMode;

		// Specifies whether the material is double sided. When this value is false, back-face culling 
		// is enabled. When this value is true, back-face culling is disabled and double sided lighting 
		// is enabled. The back-face must have its normals reversed before the lighting equation is evaluated.
		bool TwoSided;
	};
}