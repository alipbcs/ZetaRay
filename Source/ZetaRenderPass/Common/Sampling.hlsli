#ifndef SAMPLING_H
#define SAMPLING_H

#include "Math.hlsli"

// Refs: 
// 1. https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
// 2. M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2016.
// 3. P. Shirley, S. Laine, D. Hart, M. Pharr, P. Clarberg, E. Haines, M. Raab, and D. Cline, "Sampling
//    Transformations Zoo," in Ray Tracing Gems, 2019.

struct RNG
{
    static uint Pcg(uint x)
    {
        uint state = x * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    // Ref: M. Jarzynski and M. Olano, "Hash Functions for GPU Rendering," Journal of Computer Graphics Techniques, 2020.
    static uint3 Pcg3d(uint3 v)
    {
        v = v * 1664525u + 1013904223u;
        v.x += v.y * v.z;
        v.y += v.z * v.x;
        v.z += v.x * v.y;
        v ^= v >> 16u;
        v.x += v.y * v.z;
        v.y += v.z * v.x;
        v.z += v.x * v.y;
        return v;
    }

    static RNG Init(uint2 pixel, uint frame)
    {
        RNG rng;
#if 0
        rng.State = RNG::Pcg(pixel.x + RNG::Pcg(pixel.y + RNG::Pcg(frame)));
#else
        rng.State = RNG::Pcg3d(uint3(pixel, frame)).x;
#endif

        return rng;
    }

    static RNG Init(uint idx, uint frame)
    {
        RNG rng;
        rng.State = rng.Pcg(idx + Pcg(frame));

        return rng;
    }

    uint UniformUint()
    {
        this.State = this.State * 747796405u + 2891336453u;
        uint word = ((this.State >> ((this.State >> 28u) + 4u)) ^ this.State) * 277803737u;

        return (word >> 22u) ^ word;
    }

    float Uniform()
    {
        const float oneSubEps = 0x1.fffffep-1;
        const float uniformFloat01Inclusive = UniformUint() * 0x1p-32f;
        return uniformFloat01Inclusive < oneSubEps ? uniformFloat01Inclusive : oneSubEps;
    }

    // Returns samples in [0, bound)
    uint UniformUintBounded(uint bound)
    {
        uint32_t threshold = (~bound + 1u) % bound;

        for (;;) 
        {
            uint32_t r = UniformUint();

            if (r >= threshold)
                return r % bound;
        }        
    }

    // Returns samples in [0, bound). Biased but faster than #UniformUintBounded(): 
    // https://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle
    uint UniformUintBounded_Faster(uint bound)
    {
        return uint(Uniform() * float(bound));
    }

    float2 Uniform2D()
    {
        float u0 = Uniform();
        float u1 = Uniform();

        return float2(u0, u1);
    }

    float3 Uniform3D()
    {
        float u0 = Uniform();
        float u1 = Uniform();
        float u2 = Uniform();

        return float3(u0, u1, u2);
    }

    uint State;
};

//--------------------------------------------------------------------------------------
// Sampling Transformations
//--------------------------------------------------------------------------------------

namespace Sampling
{
    // Returns samples about the (0, 0, 1) axis
    float3 UniformSampleHemisphere(float2 u, out float pdf)
    {
        const float phi = TWO_PI * u.y;
        const float sinTheta = sqrt(1.0f - u.x * u.x);

        const float x = cos(phi) * sinTheta;
        const float y = sin(phi) * sinTheta;
        const float z = u.x;

        pdf = ONE_OVER_2_PI;

        return float3(x, y, z);
    }

    // Returns samples about the (0, 0, 1) axis
    float3 SampleCosineWeightedHemisphere(float2 u, out float pdf)
    {
        const float phi = TWO_PI * u.y;
        const float sinTheta = sqrt(u.x);

        const float x = cos(phi) * sinTheta;
        const float y = sin(phi) * sinTheta;
        const float z = sqrt(1.0f - u.x); // = cos(theta)

        pdf = z * ONE_OVER_PI; // = cos(theta) / PI

        return float3(x, y, z);
    }

    // Returns samples about the (0, 0, 1) axis
    float3 UniformSampleCone(float2 u, float cosThetaMax, out float pdf)
    {
        const float phi = TWO_PI * u.y;

        const float cosTheta = 1.0f - u.x + u.x * cosThetaMax;
        const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

        const float x = cos(phi) * sinTheta;
        const float y = sin(phi) * sinTheta;
        const float z = cosTheta;

        pdf = ONE_OVER_2_PI * rcp(1.0f - cosThetaMax);

        return float3(x, y, z);
    }

    float2 UniformSampleDisk(float2 u)
    {
        const float r = sqrt(u.x);
        const float phi = TWO_PI * u.y;

        return float2(r * cos(phi), r * sin(phi));
    }

    float2 UniformSampleDiskConcentricMapping(float2 u)
    {
        float a = 2.0f * u.x - 1.0f;
        float b = 2.0f * u.y - 1.0f;

        float r;
        float phi;

        if (a * a > b * b)
        {
            r = a;
            phi = PI_OVER_4 * (b / a);
        }
        else
        {
            r = b;
            phi = PI_OVER_2 - PI_OVER_4 * (a / b);
        }

        return float2(r * cos(phi), r * sin(phi));
    }

    float3 UniformSampleSphere(float2 u)
    {
        // Compute radius r (branchless).
        float u0 = 2.0f * u.x - 1.0f;
        float u1 = 2.0f * u.y - 1.0f;

        float d = 1.0f - (abs(u0) + abs(u1));
        float r = 1.0f - abs(d);

        // Compute phi in the first quadrant (branchless, except for the
        // division-by-zero test), using sign(u) to map the result to the
        // correct quadrant below.
        float phi = (r == 0) ? 0 : PI_OVER_4 * ((abs(u1) - abs(u0)) / r + 1.0f);
        float f = r * sqrt(2.0f - r * r);
        float x = f * sign(u0) * cos(phi);
        float y = f * sign(u1) * sin(phi);
        float z = sign(d) * (1.0f - r * r);

        return float3(x, y, z);
    }

    // Any point inside the triangle with vertices V0, V1, and V2 can be expressed as
    //        P = b0 * V0 + b1 * V1 + b2 * V2
    // where b0 + b1 + b2 = 1. Therefore, only b1 and b2 need to be sampled.
    float2 UniformSampleTriangle(float2 u)
    {
        // Ref: Eric Heitz. A Low-Distortion Map Between Triangle and Square. 2019.
        float b1, b2;
        
        if (u.y > u.x)
        {
            b1 = u.x * 0.5f;
            b2 = u.y - b1;
        }
        else
        {
            b2 = u.y * 0.5f;
            b1 = u.x - b2;
        }

        return float2(b1, b2);
    }
}

#endif