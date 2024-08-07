#include "GBufferRT_Common.h"
#include "../Common/FrameConstants.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/RT.hlsli"

namespace GBufferRT
{
    uint EncodeFloat2AsSNorm16(float2 u)
    {
        int16_t2 encoded = Math::EncodeAsSNorm2(u);
        return (uint(asuint16(encoded.y)) << 16) | asuint16(encoded.x);
    }

    float2 DecodeSNorm16ToFloat2(uint e)
    {
        int16_t2 encoded = int16_t2(asint16(uint16_t(e & 0xffff)), asint16(uint16_t(e >> 16)));
        return Math::DecodeSNorm2(encoded);
    }

    // Rate of change of texture uv coordinates w.r.t. screen space
    // Ref: M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering, Morgan Kaufmann, 2016.
    float4 UVDifferentials(int2 DTid, float3 origin, float3 dir, float2 jitter, 
        bool thinLens, float2 lensSample, float focusDepth, float t, float3 dpdu, 
        float3 dpdv, ConstantBuffer<cbFrameConstants> g_frame)
    {
        // 1. Form auxilary rays offset one pixel to right and above of camera ray (r_x and r_y).
        // 2. Form tangent plane at hit point (P).
        // 3. Approximae surface around hit point using a first-order approximation at P. Hit 
        //    points for auxilary rays can be solved for using the ray-plane intersection 
        //    algorithm (denote by p_x and p_y).
        // 4. Each triangle can be described as a parametric surface p = f(u, v). Since triangle 
        //    is planar, the first-order approximation is exact:
        //          p' - p0 = [dpdu dpdv] duv
        // 5. Since dpdu and dpdv for the hit triangle are known, by replacing p_x and p_y for p' 
        //    in above, ddx(uv) and ddy(uv) can be approximated by solving the linear system.

        // determinat of square matrix A^T A
        float dpduDotdpdu = dot(dpdu, dpdu);
        float dpdvDotdpdv = dot(dpdv, dpdv);
        float dpduDotdpdv = dot(dpdu, dpdv);
        float det = dpduDotdpdu * dpdvDotdpdv - dpduDotdpdv * dpduDotdpdv;
        // A^T A is not invertible
        if (abs(det) < 1e-7f)
            return 0;

        // form ray differentials
        float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

        float3 dir_cs_x = RT::GeneratePinholeCameraRay_CS(int2(DTid.x + 1, DTid.y), 
            renderDim, g_frame.AspectRatio, g_frame.TanHalfFOV, 
            g_frame.CurrCameraJitter);
        float3 dir_cs_y = RT::GeneratePinholeCameraRay_CS(int2(DTid.x, DTid.y - 1), 
            renderDim, g_frame.AspectRatio, g_frame.TanHalfFOV, 
            g_frame.CurrCameraJitter);

        // change ray directions given the lens sample
        if(thinLens)
        {
            float3 focalPoint_x = focusDepth * dir_cs_x;
            dir_cs_x = focalPoint_x - float3(lensSample, 0);

            float3 focalPoint_y = focusDepth * dir_cs_y;
            dir_cs_y = focalPoint_y - float3(lensSample, 0);
        }

        // camera space to world space
        float3 dir_x = mad(dir_cs_x.x, g_frame.CurrView[0].xyz, 
            mad(dir_cs_x.y, g_frame.CurrView[1].xyz, dir_cs_x.z * g_frame.CurrView[2].xyz));
        dir_x = normalize(dir_x);

        float3 dir_y = mad(dir_cs_y.x, g_frame.CurrView[0].xyz, 
            mad(dir_cs_y.y, g_frame.CurrView[1].xyz, dir_cs_y.z * g_frame.CurrView[2].xyz));
        dir_y = normalize(dir_y);

        // compute intersection with tangent plane at hit point for camera ray
        float3 faceNormal = normalize(cross(dpdu, dpdv));
        float3 p = origin + t * dir;
        float d = -dot(faceNormal, p);
        float numerator = -dot(faceNormal, origin) - d;
        
        // compute intersection with tangent plane for ray differentials
        float denom_x = dot(faceNormal, dir_x);
        denom_x = (denom_x < 0 ? -1 : 1) * max(abs(denom_x), 1e-8);
        float t_x = numerator / denom_x;
        float3 p_x = origin + t_x * dir_x;

        float denom_y = dot(faceNormal, dir_y);
        denom_y = (denom_y < 0 ? -1 : 1) * max(abs(denom_y), 1e-8);
        float t_y = numerator / denom_y;
        float3 p_y = origin + t_y * dir_y;

        // since the linear system described above is overdetermined, calculate
        // the least-squares solution (denote by x_hat), which minimizes the L2-norm 
        // of A x_hat - b when there's no solution:
        //      x_hat = (A^TA)^-1 A^T b
        // where A = [dpdu dpdv] and b = delta_p

        // division by determinant happens below
        float2x2 A_T_A_Inv = float2x2(dpdvDotdpdv, -dpduDotdpdv,
                                     -dpduDotdpdv, dpduDotdpdu);
        float3 dpdx = p_x - p;
        float2 A_Txb_x = float2(dot(dpdu, dpdx), dot(dpdv, dpdx));
        float2 grads_x = mul(A_T_A_Inv, A_Txb_x) / det;

        float3 dpdy = p_y - p;
        float2 A_Txb_y = float2(dot(dpdu, dpdy), dot(dpdv, dpdy));
        float2 grads_y = mul(A_T_A_Inv, A_Txb_y) / det;

        // return clamp(float4(grads_x, grads_y), 1e-7, 1e7);
        return float4(grads_x, grads_y);
    }

    void WriteToGBuffers(uint2 DTid, float t, float3 normal, float3 baseColor, float metalness, 
        float roughness,float3 emissive, float2 motionVec, float transmission, float ior, 
        float3 dpdu, float3 dpdv, float3 dndu, float3 dndv, ConstantBuffer<cbGBufferRt> g_local)
    {
        RWTexture2D<float> g_outDepth = ResourceDescriptorHeap[g_local.DepthUavDescHeapIdx];
        g_outDepth[DTid] = t;

        RWTexture2D<float2> g_outNormal = ResourceDescriptorHeap[g_local.NormalUavDescHeapIdx];
        g_outNormal[DTid] = Math::EncodeUnitVector(normal);

        RWTexture2D<float3> g_outBaseColor = ResourceDescriptorHeap[g_local.BaseColorUavDescHeapIdx];
        g_outBaseColor[DTid] = baseColor;

        RWTexture2D<float2> g_outMetallicRoughness = ResourceDescriptorHeap[g_local.MetallicRoughnessUavDescHeapIdx];
        g_outMetallicRoughness[DTid] = float2(metalness, roughness);

        if(dot(emissive, emissive) > 0)
        {
            RWTexture2D<float3> g_outEmissive = ResourceDescriptorHeap[g_local.EmissiveColorUavDescHeapIdx];
            // R11G11B10 doesn't have a sign bit, make sure passed value is non-negative
            g_outEmissive[DTid] = max(0, emissive);
        }

        if(transmission > 0)
        {
            RWTexture2D<float2> g_outTransmission = ResourceDescriptorHeap[g_local.TransmissionUavDescHeapIdx];
            float ior_unorm = GBuffer::EncodeIOR(ior);
            g_outTransmission[DTid] = float2(transmission, ior_unorm);
        }

        RWTexture2D<float2> g_outMotion = ResourceDescriptorHeap[g_local.MotionVectorUavDescHeapIdx];
        g_outMotion[DTid] = motionVec;

        RWTexture2D<uint4> g_outTriGeo_A = ResourceDescriptorHeap[g_local.TriGeoADescHeapIdx];
        RWTexture2D<uint2> g_outTriGeo_B = ResourceDescriptorHeap[g_local.TriGeoBDescHeapIdx];
        uint3 dpdu_h = asuint16(half3(dpdu));
        uint3 dpdv_h = asuint16(half3(dpdv));
        uint3 dndu_h = asuint16(half3(dndu));
        uint3 dndv_h = asuint16(half3(dndv));

        g_outTriGeo_A[DTid] = uint4(dpdu_h.x | (dpdu_h.y << 16),
            dpdu_h.z | (dpdv_h.x << 16),
            dpdv_h.y | (dpdv_h.z << 16),
            dndu_h.x | (dndu_h.y << 16));
        g_outTriGeo_B[DTid] = uint2(dndu_h.z | (dndv_h.x << 16), dndv_h.y | (dndv_h.z << 16));
    }

    float2 IntegrateBump(float2 x)
    {
        float2 f = floor(0.5f * x);
        return f + 2.0f * max((0.5f * x) - f - 0.5f, 0.0f);
    }

    float3 GetCheckerboardColor(float2 uv, float4 grads)
    {
        float2 fw = max(abs(grads.xy * 300), abs(grads.zw * 300));
        float width = max(fw.x, fw.y);

        float2 p0 = uv - 0.5 * width;
        float2 p1 = uv + 0.5 * width;

        float2 i = (IntegrateBump(p1) - IntegrateBump(p0)) / width;
        float area2 = i.x + i.y - 2 * i.x * i.y;

        return (1 - area2) * float3(0.85f, 0.6f, 0.7f) + area2 * float3(0.034f, 0.015f, 0.048f);
    }

    void ApplyTextureMaps(uint2 DTid, float z_view, float3 wo, float2 uv, uint matIdx, 
        float3 geoNormal, float3 tangent, float2 motionVec, float4 grads, float3 dpdu, 
        float3 dpdv, float3 dndu, float3 dndv, ConstantBuffer<cbFrameConstants> g_frame, 
        ConstantBuffer<cbGBufferRt> g_local, StructuredBuffer<Material> g_materials)
    {
        const Material mat = g_materials[NonUniformResourceIndex(matIdx)];
        // Apply negative mip bias when upscaling
        grads *= g_frame.CameraRayUVGradsScale;

        float3 baseColor = Math::UnpackRGBA(mat.BaseColorFactor).rgb;
        float4 emissiveColorNormalScale = Math::UnpackRGBA(mat.EmissiveFactorNormalScale);
        float2 metalnessAlphaCuttoff = Math::UnpackRG(mat.MetallicFactorAlphaCuttoff);
        float roughness = mat.GetRoughnessFactor();
        float3 shadingNormal = geoNormal;

        if (mat.BaseColorTexture != UINT32_MAX)
        {
            uint offset = NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + 
                mat.BaseColorTexture);
            BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[offset];
            baseColor *= g_baseCol.SampleGrad(g_samAnisotropicWrap, uv, grads.xy, grads.zw).rgb;
        }
        else if (dot(baseColor.rgb, 1) == 0)
        {
             baseColor.rgb = GetCheckerboardColor(uv * 300.0f, grads);
        }
        // avoid normal mapping if tangent = (0, 0, 0), which results in NaN
        const uint16_t normalTex = mat.GetNormalTex();

        if (normalTex != UINT16_MAX && abs(dot(tangent, tangent)) > 1e-6)
        {
            uint offset = NonUniformResourceIndex(g_frame.NormalMapsDescHeapOffset + normalTex);
            NORMAL_MAP g_normalMap = ResourceDescriptorHeap[offset];
            float2 bump2 = g_normalMap.SampleGrad(g_samAnisotropicWrap, uv, grads.xy, grads.zw);

            shadingNormal = Math::TangentSpaceToWorldSpace(bump2, tangent, geoNormal, 
                emissiveColorNormalScale.w);
        }

        // reverse normal for double-sided meshes if facing away from camera
        if (mat.IsDoubleSided() && dot(wo, geoNormal) < 0)
        {
            shadingNormal *= -1;
            dndu *= -1;
            dndv *= -1;
        }

        // Neighborhood around surface point (approximated by tangent plane from geometry normal) is visible, 
        // yet camera would be behind the tangent plane formed by bumped normal -- this can cause black spots 
        // as brdf evaluates to 0 in these regions. To mitigate the issue, rotate the bumped normal towards
        // wo until their dot product becomes greater than zero:
        //
        // 1. Project bumped normal onto plane formed by normal vector wo
        // 2. Slightly rotate the projection towards wo so that their dot product becomes greater than zero
#if 1
        if(dot(wo, geoNormal) > 0 && dot(wo, shadingNormal) < 0)
        {
            wo = normalize(wo);
            shadingNormal = shadingNormal - dot(shadingNormal, wo) * wo;
            shadingNormal = 1e-4f * wo + shadingNormal;
            shadingNormal = normalize(shadingNormal);
        }
#endif

        const uint16_t metallicRoughnessTex = mat.GetMetallicRoughnessTex();

        if (metallicRoughnessTex != UINT16_MAX)
        {
            uint offset = NonUniformResourceIndex(g_frame.MetallicRoughnessMapsDescHeapOffset + 
                metallicRoughnessTex);
            METALLIC_ROUGHNESS_MAP g_metallicRoughnessMap = ResourceDescriptorHeap[offset];
            float2 mr = g_metallicRoughnessMap.SampleGrad(g_samAnisotropicWrap, uv, grads.xy, grads.zw);

            metalnessAlphaCuttoff.x *= mr.x;
            roughness *= mr.y;
        }

        uint16_t emissiveTex = mat.GetEmissiveTex();
        float emissiveStrength = (float)mat.GetEmissiveStrength();

        if (emissiveTex != UINT16_MAX)
        {
            uint offset = NonUniformResourceIndex(g_frame.EmissiveMapsDescHeapOffset + emissiveTex);
            EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[offset];
            emissiveColorNormalScale.rgb *= g_emissiveMap.SampleLevel(g_samLinearWrap, uv, 0).xyz;
        }
        
        emissiveColorNormalScale.rgb *= emissiveStrength;

        // encode metalness along with some other stuff
        float tr = mat.GetTransmission();
        float ior = mat.GetIOR();
        metalnessAlphaCuttoff.x = GBuffer::EncodeMetallic(metalnessAlphaCuttoff.x, tr > 0, 
            emissiveColorNormalScale.rgb);

        WriteToGBuffers(DTid, z_view, shadingNormal, baseColor.rgb, metalnessAlphaCuttoff.x, 
            roughness, emissiveColorNormalScale.rgb, motionVec, tr, ior, dpdu, dpdv, dndu,
            dndv, g_local);
    }
}