#pragma once

#include <Utility/Span.h>
#include <Math/Vector.h>

namespace ZetaRay::Util
{
    struct RNG;
}

namespace ZetaRay::Math
{
    // Generates i'th index of the Halton low-discrepancy sequence for base b 
    float Halton(int i, int b);

    Math::float3 UniformSampleSphere(const Math::float2 u);

    struct AliasTableEntry
    {
        float P_Curr = 0.0f;
        float P_Orig = 0.0f;
        uint32_t Alias = uint32_t(-1);
    };

    // Alias Table
    // Ref: https://www.keithschwarz.com/darts-dice-coins/

    // Normalizes the given set of weights so that they sum to N where N is the size of sample space
    void AliasTable_Normalize(Util::MutableSpan<float> weights);
    // Generates an alias table for the given distribution.
    void AliasTable_Build(Util::MutableSpan<float> weights, Util::MutableSpan<AliasTableEntry> table);
    // Draws sample from the given alias table
    uint32_t SampleAliasTable(Util::Span<AliasTableEntry> table, Util::RNG& rng, float& pdf);
}
