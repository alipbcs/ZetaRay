#pragma once

#include "Common.h"
#include "../RenderPass/Common/LightSourceData.h"
#include "../Utility/SmallVector.h"

namespace ZetaRay::Math
{
	// Generates i'th index of the Halton low-discrepancy sequence for base b 
	float Halton(uint32_t i, uint32_t b) noexcept;

	// Uniformly samples the hemisphere around (0, 0, 0)
	float3 GetUniformSampleHemisphere(float2 u) noexcept;

	// Generates an Alias Table for the given probability distribution function
	// Ref: https://www.keithschwarz.com/darts-dice-coins/
	void BuildAliasTableUnnormalized(Util::Vector<float, 32>&& probs, Util::Vector<AliasTableEntry>& ret) noexcept;
	void BuildAliasTableNormalized(Util::Vector<float>&& probs, Util::Vector<AliasTableEntry>& ret) noexcept;
}
