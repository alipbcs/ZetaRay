#pragma once

#include "../Core/Vertex.h"
#include "../Math/Matrix.h"
#include "../Win32/Filesystem.h"
#include "../RenderPass/Common/Material.h"

namespace ZetaRay::Scene
{
	struct AffineTransformation
	{
		Math::float3 Scale;
		Math::float4 RotQuat;
		Math::float3 Translation;
	};

	struct Keyframe
	{
		AffineTransformation Transform;
		float Time;
	};

	// Ref: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
	// This information helps in deciding which acceleration structure flags
	// to use and how or whether to group models together in one BLAS
	enum class RT_MESH_MODE : uint8_t
	{
		// slow build time but fastest possible trace time
		STATIC = 0,

		// dynamic meshes that don't change drastically, (change in number of
		// primitives constituting the mesh, fast-moving objects, ...), can be updated and is
		// fast to rebuild
		SEMI_DYNAMIC,

		// dynamic mesh for which rebuilding is more efficient (w.r.t. acceleration
		// structure quality) than updating due to their dynamic behavior
		FULL_DYNAMIC,

		// mesh that potentially many rays would hit, fastest trace and can be updated
		PRIMARY
	};

	namespace Asset
	{
		struct MeshSubset
		{
			Util::SmallVector<Core::VertexPosNormalTexTangent> Vertices;
			Util::SmallVector<INDEX_TYPE> Indices;
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
				MetallicFactor = 0.0f;
				RoughnessFactor = 1.0f;
				NormalScale = 1.0f;
				AlphaCuttoff = 0.5f;
				AlphaMode = Material::ALPHA_MODE::OPAQUE;
				TwoSided = false;
			}

			int Index;

			// RGB specify the base color of the material (sRGB). fourth component(A) represents the linear
			// alpha coverage of the material
			Win32::Filesystem::Path BaseColorTexPath;

			// The metallic - roughness texture.The metalness values are sampled from the B channel.The roughness values are sampled
			// from the G channel.These values are linear
			Win32::Filesystem::Path MetalnessRoughnessTexPath;

			// A tangent space normal map.The texture contains RGB components in linear space. Each texel represents the 
			// XYZ components of a normal vector in tangent space. Red & Green map to [-1 to 1], Blue to Z[1 / 255 to 1]
			Win32::Filesystem::Path NormalTexPath;
			Win32::Filesystem::Path EmissiveTexPath;

			// Linear. fourth component (A) is the alpha coverage of the material (interpreted by the alphaMode property). 
			// These values are linear. multiplied baseColorTexture (if present)
			Math::float4 BaseColorFactor;

			// factors for the emissive color of the material
			Math::float3 EmissiveFactor;

			// The metalness of the material (linear) .A value of 1.0 means the material is a metal. A value 
			// of 0.0 means the material is a dielectric
			float MetallicFactor;

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
}