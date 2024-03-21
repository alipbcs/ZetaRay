#include "Vector.h"
#include "../Utility/Span.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::Util;

bool Math::SolveQuadratic(float a, float b, float c, float& x1, float& x2)
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

void Math::SphericalFromCartesian(const float3& w, float& theta, float& phi)
{
    // x = sin(theta) * cos(phi)
    // y = cos(theta)
    // z = -sin(theta) * sin(phi)
    theta = acosf(w.y);                             // [0, PI]
    // phi is measured clockwise from the x-axis and atan2f uses the sign to figure out the quadrant
    phi = atan2f(-w.z, w.x);
    phi = phi < 0.0f ? phi + Math::TWO_PI : phi;    // [0, 2 * PI]
}

float3 Math::SphericalToCartesian(float theta, float phi)
{
    // x = sin(theta) * cos(phi)
    // y = cos(theta)
    // z = -sin(theta) * sin(phi)
    float sinTheta = sinf(theta);
    return float3(sinTheta * cosf(phi), cosf(theta), -sinTheta * sinf(phi));
}

size_t Math::SubdivideRangeWithMin(size_t n, size_t maxNumGroups, MutableSpan<size_t> offsets, MutableSpan<size_t> sizes, size_t minNumElems)
{
    Assert(offsets.size() >= maxNumGroups, "out-of-bound access in offsets array.");
    Assert(sizes.size() >= maxNumGroups, "out-of-bound access in sizes array.");

    if (n == 0)
        return 0;

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

// Fast math must be disabled for Kahan summation
#pragma float_control(precise, on, push)

float Math::KahanSum(Span<float> data)
{
    const int64_t N = data.size();
    float sum = 0.0f;
    float compensation = 0.0;

    // align to 32-byte boundary
    const float* curr = data.data();
    while ((reinterpret_cast<uintptr_t>(curr) & 31) != 0)
    {
        float corrected = *curr - compensation;
        float newSum = sum + corrected;
        compensation = (newSum - sum) - corrected;
        sum = newSum;

        curr++;
    }

    // Largest multiple of 16 (each loop iteration loads two avx registers -- 16 floats) that is smaller than N
    const int64_t startOffset = curr - data.data();
    int64_t numToSumSIMD = (N - startOffset);
    numToSumSIMD -= numToSumSIMD & 15;

    const float* end = curr + numToSumSIMD;
    __m256 vSum = _mm256_setzero_ps();
    __m256 vCompensation = _mm256_setzero_ps();

    // Unroll two sums in each iteration
    for (; curr < end; curr += 16)
    {
        __m256 V1 = _mm256_load_ps(curr);
        __m256 V2 = _mm256_load_ps(curr + 8);

        // Essentially each simd lane is doing a separate Kahan summation
        __m256 vCurr = _mm256_add_ps(V1, V2);
        __m256 vCorrected = _mm256_sub_ps(vCurr, vCompensation);
        __m256 vNewSum = _mm256_add_ps(vSum, vCorrected);
        vCompensation = _mm256_sub_ps(vNewSum, vSum);
        vCompensation = _mm256_sub_ps(vCompensation, vCorrected);

        vSum = vNewSum;
    }

    // Sum the different simd lanes
    alignas(32) float simdSum[8];
    alignas(32) float simdCompensation[8];
    _mm256_store_ps(simdSum, vSum);
    _mm256_store_ps(simdCompensation, vCompensation);

    // Add the simd lanes
    for (int i = 0; i < 8; i++)
    {
        float corrected = simdSum[i] - compensation - simdCompensation[i];
        float newSum = sum + corrected;
        compensation = (newSum - sum) - corrected;
        sum = newSum;
    }

    // Add the remaining data
    for (int64_t i = startOffset + numToSumSIMD; i < N; i++)
    {
        float corrected = data[i] - compensation;
        float newSum = sum + corrected;
        compensation = (newSum - sum) - corrected;
        sum = newSum;
    }

    return sum;
}

#pragma float_control(pop)
