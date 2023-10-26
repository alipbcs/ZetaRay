#pragma once

#include "../Math/Vector.h"

namespace ZetaRay::Core
{
	// 32 bytes
	struct Vertex
	{
		Math::float3 Position;
		Math::float2 TexUV;
		Math::snorm3 Normal;
		Math::snorm3 Tangent;
	};
}