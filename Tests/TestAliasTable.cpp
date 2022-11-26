#include <Math/Sampling.h>
#include <Utility/SmallVector.h>
#include <Utility/RNG.h>
#include <Win32/App.h>
#include <doctest-2.4.9/doctest.h>

using namespace ZetaRay;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

uint32_t SampleAliasTable(Vector<AliasTableEntry>& aliasTable, RNG& rng, float& pdf)
{
	uint32_t idx = rng.GetUniformUintBounded((uint32_t)aliasTable.size());

	AliasTableEntry e = aliasTable[idx];
	float p = rng.GetUniformFloat();

	if (p <= e.P)
	{
		pdf = e.OriginalProb;
		return idx;
	}

	pdf = aliasTable[e.Alias].OriginalProb;
	return e.Alias;
}

TEST_SUITE("AliasTable")
{
	//TEST_CASE("Init")
	//{
	//	App::InitSimple();
	//}

	TEST_CASE("Normalize")
	{
		float vals[] = { 1.0f, 22.0f, 4.0f, 8.0f, 3.5f, 10.0f };
		SmallVector<float, Support::SystemAllocator, 6> vec;
		vec.append_range(vals, vals + sizeof(vals) / sizeof(float));

		SmallVector<AliasTableEntry> ret;
		BuildAliasTableUnnormalized(ZetaMove(vec), ret);

		float sum = 0.0f;
		for (auto e : ret)
		{
			sum += e.OriginalProb;
		}

		INFO("Set of values don't form a probability distribution function as they don't integrate to 1");
		CHECK(fabsf(1.0f - sum) < 1e-5f);
	}

	TEST_CASE("ReturnedPdfMatchesOriginal")
	{
		int unused;
		RNG rng(reinterpret_cast<uintptr_t>(&unused));
		INFO("RNG seed: ", reinterpret_cast<uintptr_t>(&unused));

		const uint32_t n = 1 + rng.GetUniformUintBounded(999);
		SmallVector<float> vals;
		vals.resize(n);
		float sum = 0.0f;

		for (uint32_t i = 0; i < n; i++)
		{
			float f = rng.GetUniformFloat() * 100.0f;
			vals[i] = f;
			sum += f;
		}

		for (uint32_t i = 0; i < n; i++)
			vals[i] /= sum;

		// valis is moved below, make a copy
		SmallVector<float> valsCopy = vals;

		SmallVector<AliasTableEntry> ret;
		BuildAliasTableNormalized(ZetaMove(vals), ret);

		REQUIRE(ret.size() == n);

		for (int i = 0; i < 100; i++)
		{
			float pdf;
			uint32_t idx = SampleAliasTable(ret, rng, pdf);

			INFO("Out-of-bound index");
			CHECK(idx < n);

			INFO("Density mismatch, got ", pdf, ", expected ", valsCopy[idx]);
			CHECK(pdf == valsCopy[idx]);
		}
	}

	TEST_CASE("Density")
	{
		int unused;
		RNG rng(reinterpret_cast<uintptr_t>(&unused));
		INFO("RNG seed: ", reinterpret_cast<uintptr_t>(&unused));

		const uint32_t n = 20;
		SmallVector<float> vals;
		vals.resize(n);
		float sum = 0.0f;

		for (uint32_t i = 0; i < n; i++)
		{
			float f = 1 + (float)rng.GetUniformUintBounded(99);
			vals[i] = f;
			sum += f;
		}

		for (uint32_t i = 0; i < n; i++)
			vals[i] /= sum;

		SmallVector<float> valsCopy = vals;

		SmallVector<AliasTableEntry> ret;
		BuildAliasTableNormalized(ZetaMove(vals), ret);

		REQUIRE(ret.size() == n);

		const uint32_t sampleSize = 5000;
		SmallVector<size_t> count;
		count.resize(n, 0);

		for (int i = 0; i < sampleSize; i++)
		{
			float pdf;
			uint32_t idx = SampleAliasTable(ret, rng, pdf);

			count[idx]++;
		}

		for (uint32_t i = 0; i < n; i++)
		{
			float sampleDensity = (float)count[i] / sampleSize;
			float trueDensity = valsCopy[i];

			INFO("Sample density mismatch, got ", sampleDensity, ", expected ", trueDensity);
			CHECK(fabsf(trueDensity - sampleDensity) <= 1e-2f);
		}
	}
};