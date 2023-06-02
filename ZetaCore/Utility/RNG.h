#pragma once

#include "../App/ZetaRay.h"
#include <float.h>

namespace ZetaRay::Util
{
    // Following is based on: https://github.com/mmp/pbrt-v3/blob/master/src/core/rng.h
    // 
    // pbrt source code is Copyright(c) 1998-2016
    //                    Matt Pharr, Greg Humphreys, and Wenzel Jakob.
    // Redistribution and use in source and binary forms, with or without
    // modification, are permitted provided that the following conditions are
    // met:
    //  - Redistributions of source code must retain the above copyright
    //    notice, this list of conditions and the following disclaimer.
    //  - Redistributions in binary form must reproduce the above copyright
    //    notice, this list of conditions and the following disclaimer in the
    //    documentation and/or other materials provided with the distribution.
    // 
    // THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    // IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    // TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    // PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    // HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    // SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    // LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    // DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    // THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    // (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    // OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
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
        // See https://www.pcg-random.org for more info
        uint32_t GetUniformUintBounded(uint32_t bound) noexcept
        {
            uint32_t threshold = (~bound + 1u) % bound;

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