#include "Sampling.h"
#include <Utility/RNG.h>
#include <cmath>

using namespace ZetaRay;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// Sampling
//--------------------------------------------------------------------------------------

void Math::AliasTable_Normalize(MutableSpan<float> weights)
{
	// compute the sum of weights
	const int64_t N = weights.size();
	const float sum = Math::KahanSum(weights);
	Assert(!IsNaN(sum), "sum of weights was NaN.");

	// multiply each probability by N so that mean becomes 1 instead of 1 / N
	const float sumRcp = N / sum;

	// align to 32 bytes
	float* curr = weights.data();
	while ((reinterpret_cast<uintptr_t>(curr) & 31) != 0)
	{
		*curr *= sumRcp;
		curr++;
	}

	const int64_t startOffset = curr - weights.data();

	// largest multiple of 8 that is smaller than N
	int64_t numToSumSIMD = N - startOffset;
	numToSumSIMD -= numToSumSIMD & 7;

	const float* end = curr + numToSumSIMD;
	__m256 vSumRcp = _mm256_broadcast_ss(&sumRcp);

	for (; curr < end; curr += 8)
	{
		__m256 V = _mm256_load_ps(curr);
		V = _mm256_mul_ps(V, vSumRcp);

		_mm256_store_ps(curr, V);
	}

	for (int64_t i = startOffset + numToSumSIMD; i < N; i++)
		weights[i] *= sumRcp;
}

void Math::AliasTable_Build(Util::MutableSpan<float> probs, Util::MutableSpan<AliasTableEntry> table)
{
	const int64_t N = probs.size();
	const float oneDivN = 1.0f / N;
	AliasTable_Normalize(probs);

	for (int64_t i = 0; i < N; i++)
	{
		table[i].P_Orig = probs[i] * oneDivN;
	}

	// maintain an index buffer since original ordering of elements must be preserved
	SmallVector<uint32_t> larger;
	larger.reserve(N);

	SmallVector<uint32_t> smaller;
	smaller.reserve(N);

	for (int64_t i = 0; i < N; i++)
	{
		if (probs[i] < 1.0f)
			smaller.push_back((uint32_t)i);
		else
			larger.push_back((uint32_t)i);
	}

#ifdef _DEBUG
	int64_t numInsertions = 0;
#endif // _DEBUG

	// in each iteration, pick two probabilities such that one is smaller than 1.0 and the other larger 
	// than 1.0. Use the latter to bring up the former to 1.0.
	while (!smaller.empty() && !larger.empty())
	{
		const uint32_t smallerIdx = smaller.back();
		smaller.pop_back();
		const float smallerProb = probs[smallerIdx];

		const uint32_t largerIdx = larger.back();
		float largerProb = probs[largerIdx];
		Assert(largerProb >= 1.0f, "should be >= 1.0");

		auto& e = table[smallerIdx];
		Assert(e.Alias == -1, "Every element must be inserted exactly one time.");
		e.Alias = largerIdx;
		e.P_Curr = smallerProb;

		// = largerProb - (1.0f - smallerProb);
		largerProb = (smallerProb + largerProb) - 1.0f;
		probs[largerIdx] = largerProb;

		if (largerProb < 1.0f)
		{
			larger.pop_back();
			smaller.push_back(largerIdx);
		}

#ifdef _DEBUG
		numInsertions++;
#endif
	}

	while (!larger.empty())
	{
		size_t idx = larger.back();
		larger.pop_back();
		Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

		// alias should point to itself
		table[idx].Alias = (uint32_t)idx;
		table[idx].P_Curr = 1.0f;

#ifdef _DEBUG
		numInsertions++;
#endif
	}

	while (!smaller.empty())
	{
		size_t idx = smaller.back();
		smaller.pop_back();
		Assert(fabsf(1.0f - probs[idx]) <= 0.1f, "This should be ~1.0.");

		// alias should point to itself
		table[idx].Alias = (uint32_t)idx;
		table[idx].P_Curr = 1.0f;

#ifdef _DEBUG
		numInsertions++;
#endif
	}

	Assert(numInsertions == N, "Some elements were not inserted.");
}

uint32_t Math::SampleAliasTable(Util::Span<AliasTableEntry> table, RNG& rng, float& pdf)
{
	uint32_t idx = rng.GetUniformUintBounded((uint32_t)table.size());

	AliasTableEntry s = table[idx];

	float u1 = rng.GetUniformFloat();
	if (u1 <= s.P_Curr)
	{
		pdf = s.P_Orig;
		return idx;
	}

	pdf = table[s.Alias].P_Orig;

	return s.Alias;
}

float Math::Halton(int i, int b)
{
	float f = 1.0f;
	float r = 0.0f;
	float bf = (float)b;

	while (i > 0)
	{
		f /= bf;
		r = r + f * (float)(i % b);
		i = (int)((float)i / bf);
	}

	return r;
}

float3 Math::UniformSampleSphere(float2 u)
{
	float z = 1 - 2 * u.x;
	float r = std::sqrt(Math::Max(0.0f, 1.0f - z * z));
	float phi = 2 * Math::PI * u.y;

	return float3(r * std::cos(phi), r * std::sin(phi), z);
}
