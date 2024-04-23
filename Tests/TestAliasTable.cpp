#include <Math/Sampling.h>
#include <Utility/SmallVector.h>
#include <Utility/RNG.h>
#include <App/App.h>
#include <doctest/doctest.h>

using namespace ZetaRay;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

TEST_SUITE("AliasTable")
{
    TEST_CASE("Normalize")
    {
        float vals[] = { 1.0f, 22.0f, 4.0f, 8.0f, 3.5f, 10.0f };
        SmallVector<float, Support::SystemAllocator, 6> weights;
        weights.append_range(vals, vals + sizeof(vals) / sizeof(float));

        AliasTable_Normalize(weights);

        float sum = 0.0f;
        for (auto e : weights)
            sum += e;

        INFO("Set of values don't form a probability distribution function as they don't integrate to 1.");
        CHECK(fabsf(sum - weights.size()) < 1e-7f);
    }

    TEST_CASE("ReturnedPdfMatchesOriginal")
    {
        int unused;
        RNG rng(reinterpret_cast<uintptr_t>(&unused));
        INFO("RNG seed: ", reinterpret_cast<uintptr_t>(&unused));

        const uint32_t n = 1 + rng.UniformUintBounded(999);
        SmallVector<float> vals;
        vals.resize(n);

        for (uint32_t i = 0; i < n; i++)
        {
            float f = rng.Uniform() * 100.0f;
            vals[i] = f;
        }

        SmallVector<float> valsNormalized = vals;
        const float sum = Math::KahanSum(vals);

        for (uint32_t i = 0; i < n; i++)
            valsNormalized[i] /= sum;

        SmallVector<AliasTableEntry> table;
        table.resize(n);
        AliasTable_Build(vals, table);

        for (int i = 0; i < 100; i++)
        {
            float pdf;
            uint32_t idx = SampleAliasTable(table, rng, pdf);

            INFO("Out-of-bound index");
            CHECK(idx < n);

            INFO("Density mismatch, got ", pdf, ", expected ", valsNormalized[idx]);
            CHECK(fabsf(pdf - valsNormalized[idx]) < 1e-7f);
        }
    }

    TEST_CASE("Density")
    {
        int unused;
        RNG rng(reinterpret_cast<uintptr_t>(&unused));
        INFO("RNG seed: ", reinterpret_cast<uintptr_t>(&unused));

        // generate some weights
        const uint32_t n = 50;
        SmallVector<float> vals;
        vals.resize(n);

        for (uint32_t i = 0; i < n; i++)
            vals[i] = (float)rng.UniformUintBounded(1000);

        // normalize
        SmallVector<float> valsNormalized = vals;
        const float sum = Math::KahanSum(vals);

        for (uint32_t i = 0; i < n; i++)
            valsNormalized[i] /= sum;

        SmallVector<AliasTableEntry> table;
        table.resize(n);
        AliasTable_Build(vals, table);

        const uint32_t sampleSize = 100;
        SmallVector<size_t> count;
        count.resize(n, 0);

        // generate some observations
        for (int i = 0; i < sampleSize; i++)
        {
            float pdf;
            uint32_t idx = SampleAliasTable(table, rng, pdf);

            count[idx]++;
        }

        // Chi-squared goodness-of-fit test
        double chiSquared = 0.0;
        for (size_t i = 0; i < n; ++i) 
        {
            double expected = valsNormalized[i] * sampleSize;
            double diff = count[i] - expected;
            chiSquared += (diff * diff) / expected;
        }

        // corresponding to alpha = 0.05 and dof = sampleSize - 1 = 99
        const double criticalValue = 124.34211340400407;

        INFO("Test statistic: ", chiSquared, ", critical value: ", criticalValue);
        CHECK(chiSquared <= criticalValue);
    }
};