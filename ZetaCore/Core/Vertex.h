#pragma once

#include "../Math/Vector.h"

namespace ZetaRay::Core
{
	struct Vertex
	{
		Math::float3 Position;
		Math::half3 Normal;
		Math::float2 TexUV;
		Math::half3 Tangent;
	};
}