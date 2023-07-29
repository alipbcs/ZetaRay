#pragma once

#include "Vector.h"
#include "../Utility/SmallVector.h"

namespace ZetaRay::Math
{
	// Generates i'th index of the Halton low-discrepancy sequence for base b 
	float Halton(int i, int b) noexcept;

	// Uniformly samples the hemisphere around (0, 0, 0)
	float3 GetUniformSampleHemisphere(float2 u) noexcept;

    struct AliasTableEntry
    {
        AliasTableEntry() noexcept
            : P(0.0f),
            Alias(uint32_t(-1)),
            OriginalProb(0.0f)
        {}

        float P;
        uint32_t Alias;
        float OriginalProb;

        float pad;
    };

	// Generates an Alias Table for the given probability distribution function
	// Ref: https://www.keithschwarz.com/darts-dice-coins/
	void BuildAliasTableUnnormalized(Util::Vector<float>&& probs, Util::Vector<AliasTableEntry>& ret) noexcept;
	void BuildAliasTableNormalized(Util::Vector<float>&& probs, Util::Vector<AliasTableEntry>& ret) noexcept;
}
