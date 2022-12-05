#include "Sampling.h"
#include "../Utility/Error.h"
#include <algorithm>

using namespace ZetaRay;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

namespace
{
	void Normalize(float* vals, size_t n) noexcept
	{
		// 1. compute sum of values (S)
		// 2. normalize

		// find the largest multiple of 8 that is smaller than N
		size_t numToSumSIMD = n - (n & (8 - 1));
		float sum = 0.0f;

		if (n >= 16)
		{
			Assert((reinterpret_cast<uintptr_t>(vals) & 31) == 0, "input must be 32-byte aligned.");

			// step 1
			__m256 vSum = _mm256_setzero_ps();
			const float* end = vals + numToSumSIMD;

			for (float* curr = vals; curr < end; curr += 16)
			{
				__m256 V1 = _mm256_load_ps(curr);
				__m256 V2 = _mm256_load_ps(curr + 8);

				vSum = _mm256_add_ps(V1, V2);
			}

			alignas(32) float simdSum[8];
			_mm256_store_ps(simdSum, vSum);

			for (int i = 0; i < 8; i++)
				sum += simdSum[i];

			for (size_t i = numToSumSIMD; i < n; i++)
				sum += vals[i];

			// step 2
			float sumRcp = 1.0f / sum;
			vSum = _mm256_broadcast_ss(&sumRcp);

			for (float* curr = vals; curr < end; curr += 8)
			{
				__m256 V = _mm256_load_ps(curr);
				V = _mm256_mul_ps(V, vSum);

				_mm256_store_ps(curr, V);
			}

			for (size_t i = numToSumSIMD; i < n; i++)
				vals[i] *= sumRcp;
		}
		else
		{
			for (size_t i = 0; i < n; i++)
				sum += vals[i];

			float sumRcp = 1.0f / sum;

			for (int i = 0; i < n; i++)
				vals[i] *= sumRcp;
		}
	}

	void BuildAliasTable(float* probs, size_t n, Vector<AliasTableEntry>& ret)
	{
		ret.resize(n);

		// multiply each probability by n for better numerical stability, so that mean becomes 1 instead of 1 / n
		for (size_t i = 0; i < n; i++)
		{
			ret[i].OriginalProb = probs[i];
			probs[i] *= n;
		}

		// maintain an index buffer since original ordering of elements must be retained
		SmallVector<size_t> larger;
		SmallVector<size_t> smaller;

		for (size_t i = 0; i < n; i++)
		{
			if (probs[i] < 1.0f)
				smaller.push_back(i);
			else
				larger.push_back(i);
		}

#ifdef _DEBUG
		size_t numInsertions = 0;
#endif // _DEBUG

		// in each iteration, pick two probabilities such that one is smaller than 1 and the other one larger 
		// than 1. Use the latter to bring up the former to 1.
		while (!smaller.empty())
		{
			const size_t smallerIdx = smaller.back();
			smaller.pop_back();

			float smallerProb = probs[smallerIdx];
			if (smallerProb > 1.0f - 1e-5f)
				break;

			const size_t largerIdx = larger.back();
			float largerProb = probs[largerIdx];
			Assert(largerProb >= 1.0f - 1e-3f, "this should be >= 1.0");

			AliasTableEntry& e = ret[smallerIdx];
			Assert(ret[smallerIdx].Alias == -1, "Every element must be inserted exactly one time.");
			e.Alias = (uint32_t)largerIdx;
			e.P = smallerProb;

			largerProb = smallerProb + largerProb - 1.0f;		// == largerProb - (1.0f - smallerProb);
			probs[largerIdx] = largerProb;

			if (largerProb < 1.0f - 1e-3f)
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
			Assert(fabsf(1.0f - probs[idx]) <= 1e-2f, "This should be ~1.0");

			ret[idx].Alias = (uint32_t)idx;		// alias should point to itself
			ret[idx].P = 1.0f;

#ifdef _DEBUG
			numInsertions++;
#endif
		}

		Assert(numInsertions == n, "Some elements were not inserted");
	}
}

//--------------------------------------------------------------------------------------
// Sampling
//--------------------------------------------------------------------------------------

float Math::Halton(uint32_t i, uint32_t b) noexcept
{
	float f = 1.0f;
	float r = 0.0f;
	float bf = (float)b;

	while (i > 0)
	{
		f /= bf;
		r = r + f * (float)(i % b);
		i = (uint32_t)floorf((float)i / bf);
	}

	return r;
}

float3 Math::GetUniformSampleHemisphere(float2 u) noexcept
{
	float phi = TWO_PI * u.x;
	float cosTheta = u.y;
	float sinTheta = sqrtf(1.0f - cosTheta * cosTheta);

	//		return DirectX::XMFLOAT3(sinTheta * cosf(phi), cosTheta, sinTheta * sinf(phi));
	return float3(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);
}

void Math::BuildAliasTableUnnormalized(Vector<float>&& probs, Vector<AliasTableEntry>& ret) noexcept
{
	Normalize(probs.data(), probs.size());
	BuildAliasTable(probs.data(), probs.size(), ret);
}

void Math::BuildAliasTableNormalized(Vector<float>&& probs, Vector<AliasTableEntry>& ret) noexcept
{
	BuildAliasTable(probs.data(), probs.size(), ret);
}

/* Note: lost track of this function (old, buggy, etc)
std::vector<AliasTableEntry> Math::BuildAliasTableNormalized(float* probs, int n) noexcept
{
	//uint32_t N = (uint32_t)probs.size();
	Assert(((uintptr_t)(probs) & (32 - 1)) == 0, "probs should be 32-byte aligned.");

	//Check(probs.size() < std::numeric_limits<uint32_t>::max(),
	//	"Alias Table with number of elements larger than max possible uint32_t is not supported.",
	//	SEVERITY::FATAL);

	// find the largest multiple of 8 that is smaller than N
	int numToSumSIMD = n - (n & (8 - 1));
	for (int i = 0; i < numToSumSIMD; i++)
	{

	}

#ifdef _DEBUG
	float sum = 0.0;
	for (float f : probs)
		sum += f;

	Check(fabs(sum - 1.0) < 1e-6,
		"Given proability array is not a probability distribution as it does not sum to one.",
		SEVERITY::FATAL);
#endif // _DEBUG

	uint32_t numElements = (uint32_t)probs.size();
	std::vector<AliasTableEntry> ret(numElements);
	//float avg = 1.0f / numElements;
	float avg = 1.0f;	// better numerical stability when comparing to 1.0 rather than 1 / numElem ?

	std::stack<uint32_t, std::vector<uint32_t>> smaller;
	std::stack<uint32_t, std::vector<uint32_t>> larger;

	for (uint32_t i = 0; i < numElements; ++i)
	{
		ret[i].OriginalP = probs[i];
	}

	// partition elements into two collections; smaller holds the index of elements wiht prob less than average
	// larger holds the index of elements wiht prob more than or equal to average
	for (uint32_t i = 0; i < numElements; ++i)
	{
		float prob = probs[i];

		if (prob * numElements < avg)
			smaller.push(i);
		else
			larger.push(i);
	}

	// Following two loop execute exactly numElements times (in total).
	// At each iteration the invariant is maintained that sum of elements not in ret == avg.
	// In each iteration, total prob equalt to avg is removed and moved to ret at index i where i
	// is equal to to prob originally in index i of input times numElement and alias corresponds
	// to index of element that was used to top it off to avg.
	// !larger.empty() is added for numerical stability
	while (!smaller.empty() && !larger.empty())
	{
		uint32_t currSmallerIdx = smaller.top();
		smaller.pop();
		float smallerProb = probs[currSmallerIdx];

		uint32_t currLargerIdx = larger.top();
		larger.pop();
		float largerProb = probs[currLargerIdx];

		AliasTableEntry& e = ret[currSmallerIdx];
		e.Alias = currLargerIdx;
		e.P = smallerProb * numElements;

		Assert(ret[currSmallerIdx].Alias == -1, "Every element must be inserted exactly once.");

		largerProb = largerProb - (avg - smallerProb);
		probs[currLargerIdx] = largerProb;

		if (largerProb < avg)
			smaller.push(currLargerIdx);
		else
			larger.push(currLargerIdx);
	}

	while (!larger.empty())
	{
		uint32_t currLargerIdx = larger.top();
		larger.pop();
		float largerProb = probs[currLargerIdx];

		Assert(fabs(largerProb - avg) < 1e-6, "At this point all remaining probs in larger must be exactly avg.");
		Assert(ret[currLargerIdx].Alias == -1, "Every element must be inserted exactly onec.");

		AliasTableEntry& e = ret[currLargerIdx];
		e.Alias = currLargerIdx;
		e.P = 1.0f;		// prob(==avg) * numElems
	}

	Check(smaller.empty(),
		"'smaller' should be empty at this point, this means there were numerical instability.",
		SEVERITY::WARNING);

	while (!smaller.empty())
	{
		uint32_t currSmallerIdx = smaller.top();
		smaller.pop();
		float smallerProb = probs[currSmallerIdx];

		Assert(fabs(smallerProb - avg) < 1e-6, "At this point all remaining probs in smaller must be exactly avg.");
		Assert(ret[currSmallerIdx].Alias == -1, "Every element must be inserted exactly onec.");

		AliasTableEntry& e = ret[currSmallerIdx];
		e.Alias = currSmallerIdx;
		e.P = 1.0f;		// prob(==avg) * numElems
	}

	return ret;
}
*/