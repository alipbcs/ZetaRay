#pragma once

#include "../Math/Vector.h"

namespace ZetaRay::Core
{
	struct Vertex
	{
		Vertex() noexcept = default;

		Vertex(const Math::float3& position,
			const Math::float3& normal, 
			const Math::float2& texUV, 
			const Math::float3& tangent) noexcept
			: Position(position),
			Normal(normal),
			TexUV(texUV),
			Tangent(tangent)
		{}

		Math::float3 Position;
		Math::float3 Normal;
		Math::float2 TexUV;
		Math::float3 Tangent;
	};
}