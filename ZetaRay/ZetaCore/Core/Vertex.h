#pragma once

#include "../Math/Vector.h"

namespace ZetaRay::Core
{
	struct Vertex
	{
		Math::float3 Position;
		Math::snorm3 Normal;
		Math::float2 TexUV;
		Math::snorm3 Tangent;
	};
}