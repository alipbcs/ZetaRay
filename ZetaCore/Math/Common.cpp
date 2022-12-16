#include "Common.h"
#include "Vector.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;

bool Math::SolveQuadratic(float a, float b, float c, float& x1, float& x2) noexcept
{
	float delta = b * b - 4.0f * a * c;

	if (delta < 0.0f)
		return false;

	delta = sqrtf(delta);

	// Ref: https://www.pbr-book.org/3ed-2018/Utilities/Mathematical_Routines#Quadratic
	float q = b < 0 ? -0.5f * (b - delta) : -0.5f * (b + delta);

	x1 = q / a;
	x2 = c / q;

	return true;
}

void Math::SphericalFromCartesian(const float3& w, float& theta, float& phi) noexcept
{
	// x = sin(theta) * cos(phi)
	// y = cos(theta)
	// z = sin(theta) * sin(phi)
	theta = acosf(w.y);							// [0, PI]
	phi = atan2f(w.z, w.x);				
	phi = phi < 0.0f ? phi + Math::PI : phi;	// [0, 2 * PI]
}

float3 Math::SphericalToCartesian(float theta, float phi) noexcept
{
	// x = sin(theta) * cos(phi)
	// y = cos(theta)
	// z = sin(theta) * sin(phi)
	return float3(sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi));
}

size_t Math::SubdivideRangeWithMin(size_t n, size_t maxNumGroups, size_t* offsets, size_t* sizes, size_t minNumElems) noexcept
{
	size_t groupSize = Max(n / maxNumGroups, minNumElems);
	size_t actualNumGroups = Max(n / groupSize, 1llu);

	for (size_t i = 0; i < actualNumGroups; i++)
	{
		offsets[i] = i * groupSize;
		sizes[i] = groupSize;
	}

	sizes[actualNumGroups - 1] = n - offsets[actualNumGroups - 1];
	Assert(offsets[actualNumGroups - 1] + sizes[actualNumGroups - 1] == n, "bug");

	return actualNumGroups;
}
