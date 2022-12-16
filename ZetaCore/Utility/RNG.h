#pragma once

#include "../App/ZetaRay.h"
#include <float.h>

namespace ZetaRay::Util
{
    // Source: PCG (https://www.pcg-random.org/)
    // "PCG is a family of simple fast space-efficient statistically good algorithms for random number generation."
    struct RNG
    {
        // Seeds the rng. stream ID specifies which sequence to use
        explicit RNG(uint64_t streamID = 0xda3e39cb94b95bdbULL) noexcept
        {
            State = 0U;
            Inc = (streamID << 1u) | 1u;
            GetUniformUint();

            // state initializer is fixed
            State += 0x853c49e6748fea9bULL;
            GetUniformUint();
        }

        // Generates a uniformly distributed 32-bit random number
        uint32_t GetUniformUint() noexcept
        {
            uint64_t oldstate = State;
            State = oldstate * 6364136223846793005ULL + Inc;

            // Note: (uint32_t)((oldstate >> 18u) ^ oldstate) >> 27u leads to disaster!
            uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
            uint32_t rot = oldstate >> 59u;

            return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
        }

        // Generates a uniformly distributed float in [0, 1)
        float GetUniformFloat() noexcept
        {
            constexpr float oneSubEps = 0x1.fffffep-1;
            const float uniformFloat01Inclusive = GetUniformUint() * 0x1p-32f;
            
            return uniformFloat01Inclusive < oneSubEps ? uniformFloat01Inclusive : oneSubEps;
        }

        // Generates a uniformly distributed number, r, where 0 <= r < bound
        uint32_t GetUniformUintBounded(uint32_t bound) noexcept
        {
            // To avoid bias, we need to make the range of the RNG a multiple of
            // bound, which we do by dropping output less than a threshold.
            // A naive scheme to calculate the threshold would be to do
            //
            //     uint32_t threshold = 0x100000000ull % bound;
            //
            // but 64-bit div/mod is slower than 32-bit div/mod (especially on
            // 32-bit platforms).  In essence, we do
            //
            //     uint32_t threshold = (0x100000000ull-bound) % bound;
            //
            // because this version will calculate the same modulus, but the LHS
            // value is less than 2^32.

            uint32_t threshold = (~bound + 1u) % bound;

            // Uniformity guarantees that this loop will terminate.  In practice, it
            // should usually terminate quickly; on average (assuming all bounds are
            // equally likely), 82.25% of the time, we can expect it to require just
            // one iteration.  In the worst case, someone passes a bound of 2^31 + 1
            // (i.e., 2147483649), which invalidates almost 50% of the range.  In 
            // practice, bounds are typically small and only a tiny amount of the range
            // is eliminated.
            for (;;) 
            {
                uint32_t r = GetUniformUint();

                if (r >= threshold)
                    return r % bound;
            }
        }

    private:
        // RNG state
        uint64_t State;      // All values are possible.
        uint64_t Inc;        // Controls which RNG sequence (stream) is selected. Must *always* be odd.
    };
}