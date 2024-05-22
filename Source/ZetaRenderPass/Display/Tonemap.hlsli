#include "../Common/StaticTextureSamplers.hlsli"

namespace Tonemap
{
    //--------------------------------------------------------------------------------------
    // Tony McMapface
    // Ref: https://github.com/h3r2tic/tony-mc-mapface
    //--------------------------------------------------------------------------------------

    float3 tony_mc_mapface(float3 stimulus, uint lutDescHeapIdx)
    {
        // Apply a non-linear transform that the LUT is encoded with.
        const float3 encoded = stimulus / (stimulus + 1.0);

        // Align the encoded range to texel centers.
        const float LUT_DIMS = 48.0;
        const float3 uv = encoded * ((LUT_DIMS - 1.0) / LUT_DIMS) + 0.5 / LUT_DIMS;

        // Note: for OpenGL, do `uv.y = 1.0 - uv.y`
        Texture3D<float3> g_lut = ResourceDescriptorHeap[lutDescHeapIdx];
        return g_lut.SampleLevel(g_samLinearClamp, uv, 0);
    }

    //--------------------------------------------------------------------------------------
    // AgX
    // Ref: https://iolite-engine.com/blog_posts/minimal_agx_implementation
    //--------------------------------------------------------------------------------------

    float3 agxDefaultContrastApprox(float3 x) 
    {
        float3 x2 = x * x;
        float3 x4 = x2 * x2;
        float3 x6 = x4 * x2;

        return -17.86  * x6 * x
            + 78.01    * x6
            - 126.7    * x4 * x
            + 92.06    * x4
            - 28.72    * x2 * x
            + 4.361    * x2
            - 0.1718   * x
            + 0.002857;
    }

    float3 agxInset(float3 val) 
    {
        const float3x3 agx_mat = float3x3(
            0.842479062253094, 0.0423282422610123, 0.0423756549057051,
            0.0784335999999992, 0.878468636469772, 0.0784336,
            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
        
        const float min_ev = -12.47393f;
        const float max_ev = 4.026069f;

        // Input transform (inset)
        val = mul(val, agx_mat);
    
        // Log2 space encoding
        val = clamp(log2(val), min_ev, max_ev);
        val = (val - min_ev) / (max_ev - min_ev);

        // Apply sigmoid function approximation
        val = agxDefaultContrastApprox(val);

        return val;
    }

    float3 agxEotf(float3 val) 
    {
        const float3x3 agx_mat_inv = float3x3(
            1.19687900512017, -0.0528968517574562, -0.0529716355144438,
            -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
            -0.0990297440797205, -0.0989611768448433, 1.15107367264116);

        // Inverse input transform (outset)
        val = mul(val, agx_mat_inv);

        // sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
        // NOTE: We're linearizing the output here. Comment/adjust when
        // *not* using a sRGB render target
        val = pow(val, 2.2);

        return val;
    }

    float3 agxLook(float3 val, float offset, float3 slope, float power, float saturation)
    {
        const float3 lw = float3(0.2126, 0.7152, 0.0722);
        float luma = dot(val, lw);
        // ASC CDL
        val = pow(val * slope + offset, power);

        return luma + saturation * (val - luma);       
    }

    float3 AgX_Default(float3 value)
    {
        float3 tonemapped = agxInset(value);
        tonemapped = agxEotf(tonemapped);

        return tonemapped;
    }

    float3 AgX_Golden(float3 value, float saturation = 0.8f)
    {
        float3 tonemapped = agxInset(value);
        float offset = 0.0;
        float3 slope = float3(1.0, 0.9, 0.5);
        float power = 0.8;
        tonemapped = agxLook(tonemapped, offset, slope, power, saturation);
        tonemapped = agxEotf(tonemapped);

        return tonemapped;
    }

    float3 AgX_Punchy(float3 value, float saturation = 1.4f)
    {
        float3 tonemapped = agxInset(value);
        float offset = 0.0;
        float slope = 1.0;
        float power = 1.35;
        tonemapped = agxLook(tonemapped, offset, slope, power, saturation);
        tonemapped = agxEotf(tonemapped);

        return tonemapped;
    }
}