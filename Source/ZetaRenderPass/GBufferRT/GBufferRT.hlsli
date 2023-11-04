#include "GBufferRT_Common.h"
#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "../../ZetaCore/Core/Material.h"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/RT.hlsli"

namespace GBufferRT
{
    int EncodeFloat2AsSNorm16(float2 u)
    {
        int16_t2 encoded = Math::Encoding::EncodeAsSNorm2(u);
        return (uint(asuint16(encoded.y)) << 16) | asuint16(encoded.x);
    }

    float2 DecodeSNorm16ToFloat2(int e)
    {
        int16_t2 encoded = int16_t2(asint16(uint16_t(e & 0xffff)), asint16(uint16_t(e >> 16)));
        return Math::Encoding::DecodeSNorm2(encoded);
    }

    float3 TransformTRS(float3 pos, float3 translation, float4 rotation, float3 scale)
    {
        float3 transformed = pos * scale;
        transformed = Math::Transform::RotateVector(transformed, rotation);
        transformed += translation;

        return transformed;
    }

    float3 InverseTransformTRS(float3 pos, float3 translation, float4 rotation, float3 scale)
    {
        float3 transformed = pos - translation;
        float4 q_conjugate = float4(-rotation.xyz, rotation.w);
        transformed = Math::Transform::RotateVector(transformed, q_conjugate);
        transformed *= 1.0f / scale;

        return transformed;
    }

    // Ref: M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering, Morgan Kaufmann, 2016.
    void Triangle_dpdu_dpdv(float3 p0, float3 p1, float3 p2, float2 uv0, float2 uv1, float2 uv2, 
        out float3 dpdu, out float3 dpdv)
    {
        float2 duv10 = uv1 - uv0;
        float2 duv20 = uv2 - uv0;
        float3 dp10 = p1 - p0;
        float3 dp20 = p2 - p0;
        
        float det = duv10.x * duv20.y - duv10.y * duv20.x;
        if (abs(det) < 1e-8f)
        {
            float3 normal = normalize(cross(p1 - p0, p2 - p0));
            Math::Transform::revisedONB(normal, dpdu, dpdv);
            return;
        }

        float invdet = 1.0 / det;
        dpdu = (duv20.y * dp10 - duv10.y * dp20) * invdet;
        dpdv = (-duv20.x * dp10 + duv10.x * dp20) * invdet;
    }

    // rate of change of uv coordinates w.r.t. screen space
    // Ref: M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering, Morgan Kaufmann, 2016.
    float4 UVDifferentials(int2 DTid, float3 origin, float3 dir, float2 jitter, float t, float3 dpdu, float3 dpdv, 
        ConstantBuffer<cbFrameConstants> g_frame)
    {
        // 1. form auxilary rays offset one pixel to the right and above of the main ray (r_x and r_y)
        // 2. form the tangent plane at hit point (P)
        // 3. approximating the surface around hit point using a first-order approximation there (same as P),
        // hit points for auxilary rays can be solved for using the ray-plane intersection algorithm (p_x and p_y)
        // 4. each triangle can be described as a parametric surface p = f(u, v). as triangle is planar,
        // the first-order approximation is exact:
        //      p' - p0 = [dpdu dpdv] duv
        // 5. since dpdu and dpdv are known, by replacing p_x and p_y for p' in above, ddx(uv) and ddy(uv) can 
        // be approximated by solving the linear system

        float3 faceNormal = normalize(cross(dpdu, dpdv));
        
        int dim[2] = {0, 1};
        if(abs(faceNormal.x) > abs(faceNormal.y) && abs(faceNormal.x) > abs(faceNormal.z))
        {
            dim[0] = 1;
            dim[1] = 2;
        }
        else if(abs(faceNormal.y) > abs(faceNormal.z))
        {
            dim[0] = 0;
            dim[1] = 2;
        }

        float2x2 A = float2x2(dpdu[dim[0]], dpdv[dim[0]],
                              dpdu[dim[1]], dpdv[dim[1]]);
        
        float det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
        // since A is not invertible, linear system doesn't have a solution
        if (abs(det) < 1e-8f)
            return 0;

        float3 p = origin + t * dir;
        float d = -dot(faceNormal, p);
        
        // form ray differentials
        float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

        float3 dir_x = RT::GeneratePinholeCameraRay(int2(DTid.x + 1, DTid.y), 
            renderDim, 
            g_frame.AspectRatio, 
            g_frame.TanHalfFOV, 
            g_frame.CurrView[0].xyz, 
            g_frame.CurrView[1].xyz, 
            g_frame.CurrView[2].xyz,
            jitter);
        float3 dir_y = RT::GeneratePinholeCameraRay(int2(DTid.x, DTid.y - 1), 
            renderDim, 
            g_frame.AspectRatio, 
            g_frame.TanHalfFOV, 
            g_frame.CurrView[0].xyz, 
            g_frame.CurrView[1].xyz, 
            g_frame.CurrView[2].xyz,
            jitter);
        
        // compute intersection with tangent plane at hit point
        float numerator = -dot(faceNormal, origin) - d;
        float denom_x = dot(faceNormal, dir_x);
        denom_x = (denom_x < 0 ? -1 : 1) * max(abs(denom_x), 1e-8);
        float t_x = numerator / denom_x;
        float3 p_x = origin + t_x * dir_x;

        // solve the linear system for ddx(uv) and ddy(uv).
        float2 b_x = float2(p_x[dim[0]] - p[dim[0]], p_x[dim[1]] - p[dim[1]]);
        float2 grads_x;
        grads_x.x = (A[1][1] * b_x[0] - A[0][1] * b_x[1]) / det;
        grads_x.y = (A[0][0] * b_x[1] - A[1][0] * b_x[0]) / det;

        float denom_y = dot(faceNormal, dir_y);
        denom_y = (denom_y < 0 ? -1 : 1) * max(abs(denom_y), 1e-8);
        float t_y = numerator / denom_y;
        float3 p_y = origin + t_y * dir_y;
        float2 b_y = float2(p_y[dim[0]] - p[dim[0]], p_y[dim[1]] - p[dim[1]]);
        float2 grads_y;
        grads_y.x = (A[1][1] * b_y[0] - A[0][1] * b_y[1]) / det;
        grads_y.y = (A[0][0] * b_y[1] - A[1][0] * b_y[0]) / det;

        return float4(grads_x, grads_y);
    }

    void WriteToGBuffers(uint2 DTid, float t, float3 normal, float3 baseColor, float metalness, float roughness,
        float3 emissive, float2 motionVec, ConstantBuffer<cbGBufferRt> g_local)
    {
        RWTexture2D<float> g_outDepth = ResourceDescriptorHeap[g_local.DepthUavDescHeapIdx];
        g_outDepth[DTid] = t;

        RWTexture2D<float2> g_outNormal = ResourceDescriptorHeap[g_local.NormalUavDescHeapIdx];
        g_outNormal[DTid] = Math::Encoding::EncodeUnitVector(normal);

        RWTexture2D<float3> g_outBaseColor = ResourceDescriptorHeap[g_local.BaseColorUavDescHeapIdx];
        g_outBaseColor[DTid] = baseColor;

        RWTexture2D<float2> g_outMetallicRoughness = ResourceDescriptorHeap[g_local.MetallicRoughnessUavDescHeapIdx];
        g_outMetallicRoughness[DTid] = float2(metalness, roughness);

        RWTexture2D<float3> g_outEmissive = ResourceDescriptorHeap[g_local.EmissiveColorUavDescHeapIdx];
        // R11G11B10 doesn't have a sign bit, make sure passed value is non-negative
        g_outEmissive[DTid] = max(0, emissive);

        RWTexture2D<float2> g_outMotion = ResourceDescriptorHeap[g_local.MotionVectorUavDescHeapIdx];
        g_outMotion[DTid] = motionVec;
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

    void ApplyTextureMaps(uint2 DTid, float t, float2 uv, uint matIdx, float3 normal, float3 tangent, 
        float2 motionVec, float4 grads, float3 posW, ConstantBuffer<cbFrameConstants> g_frame, 
        ConstantBuffer<cbGBufferRt> g_local, StructuredBuffer<Material> g_materials)
    {
        const Material mat = g_materials[NonUniformResourceIndex(matIdx)];
        grads *= g_frame.MipBias;

        float3 baseColor = Math::Color::UnpackRGBA(mat.BaseColorFactor).rgb;
        float4 emissiveColorNormalScale = Math::Color::UnpackRGBA(mat.EmissiveFactorNormalScale);
        float2 metalnessAlphaCuttoff = Math::Color::UnpackRG(mat.MetallicFactorAlphaCuttoff);
        float roughness = mat.RoughnessFactor;

        if (mat.BaseColorTexture != -1)
        {
            BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + mat.BaseColorTexture)];
            baseColor *= g_baseCol.SampleGrad(g_samAnisotropicWrap, uv, grads.xy, grads.zw).rgb;
        }
    	else if (dot(baseColor.rgb, 1) == 0)
    	{
    	 	baseColor.rgb = GetCheckerboardColor(uv * 300.0f, grads);
    	}
        // avoid normal mapping if tangent = (0, 0, 0), which results in NaN
        if (mat.NormalTexture != -1 && abs(dot(tangent, tangent)) > 1e-6)
        {
            NORMAL_MAP g_normalMap = ResourceDescriptorHeap[NonUniformResourceIndex(g_frame.NormalMapsDescHeapOffset + mat.NormalTexture)];
            float2 bump2 = g_normalMap.SampleGrad(g_samAnisotropicWrap, uv, grads.xy, grads.zw);

            normal = Math::Transform::TangentSpaceToWorldSpace(bump2, tangent, normal, emissiveColorNormalScale.w);
        }

        if (mat.IsDoubleSided())
        {
            float3 posV = mul(g_frame.CurrView, float4(posW, 1)).xyz;
            float3 normalV = mul((float3x3) g_frame.CurrView, normal);
        
            if (dot(normalV, -posV) < 0)
                normal *= -1;
        }

        if (mat.MetallicRoughnessTexture != -1)
        {
            uint offset = NonUniformResourceIndex(g_frame.MetallicRoughnessMapsDescHeapOffset + mat.MetallicRoughnessTexture);
            METALLIC_ROUGHNESS_MAP g_metallicRoughnessMap = ResourceDescriptorHeap[offset];
            float2 mr = g_metallicRoughnessMap.SampleGrad(g_samAnisotropicWrap, uv, grads.xy, grads.zw);

            metalnessAlphaCuttoff.x *= mr.x;
            roughness *= mr.y;
        }

        uint16_t emissiveTex = mat.GetEmissiveTex();
        float emissiveStrength = mat.GetEmissiveStrength();

        if (emissiveTex != -1)
        {
            EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[NonUniformResourceIndex(g_frame.EmissiveMapsDescHeapOffset + emissiveTex)];
            emissiveColorNormalScale.rgb *= g_emissiveMap.SampleLevel(g_samLinearWrap, uv, 0).xyz;
        }
        
        emissiveColorNormalScale.rgb *= emissiveStrength;

        // encode metalness along with some other stuff
        metalnessAlphaCuttoff.x = GBuffer::EncodeMetallic(metalnessAlphaCuttoff.x, mat.BaseColorTexture, 
            emissiveColorNormalScale.rgb);
        
        WriteToGBuffers(DTid, t, normal, baseColor.rgb, metalnessAlphaCuttoff.x, roughness, 
            emissiveColorNormalScale.rgb, motionVec, g_local);
    }
}