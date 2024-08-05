#include "GBufferRT.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/Sampling.hlsli"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbGBufferRt> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);
StructuredBuffer<RT::MeshInstance> g_frameMeshData : register(t1);
StructuredBuffer<Vertex> g_sceneVertices : register(t2);
StructuredBuffer<uint> g_sceneIndices : register(t3);
StructuredBuffer<Material> g_materials : register(t4);
RWStructuredBuffer<uint> g_pick : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

struct RayPayload
{
    float t;
    float3 normal;
    float3 tangent;
    float2 uv;
    uint matIdx;
    float2 prevPosNDC;
    float3 dpdu;
    float3 dpdv;
    float3 dndu;
    float3 dndv;
    uint hitMeshIdx;
};

bool TestOpacity(uint geoIdx, uint instanceID, uint primIdx, float2 bary)
{
    const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(geoIdx + instanceID)];

    float2 alphaFactor_cutoff = Math::UnpackRG(meshData.AlphaFactor_Cutoff);
    if(alphaFactor_cutoff.y == 1.0)
        return false;

    float alpha = alphaFactor_cutoff.x;

    if(meshData.BaseColorTex != Material::INVALID_ID)
    {
        uint tri = primIdx * 3;
        tri += meshData.BaseIdxOffset;
        uint i0 = g_sceneIndices[NonUniformResourceIndex(tri)] + meshData.BaseVtxOffset;
        uint i1 = g_sceneIndices[NonUniformResourceIndex(tri + 1)] + meshData.BaseVtxOffset;
        uint i2 = g_sceneIndices[NonUniformResourceIndex(tri + 2)] + meshData.BaseVtxOffset;

        Vertex V0 = g_sceneVertices[NonUniformResourceIndex(i0)];
        Vertex V1 = g_sceneVertices[NonUniformResourceIndex(i1)];
        Vertex V2 = g_sceneVertices[NonUniformResourceIndex(i2)];

        float2 uv = V0.TexUV + bary.x * (V1.TexUV - V0.TexUV) + bary.y * (V2.TexUV - V0.TexUV);

        uint descHeadpIdx = NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + meshData.BaseColorTex);
        BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[descHeadpIdx];
        alpha *= g_baseCol.SampleLevel(g_samLinearWrap, uv, 0).a;
    }

    if (alpha < alphaFactor_cutoff.y) 
        return false;

    return true;
}

RayPayload TracePrimaryHit(float3 origin, float3 dir)
{
    RayDesc cameraRay;
    cameraRay.Origin = origin;
    cameraRay.TMin = 0;
    cameraRay.TMax = FLT_MAX;
    cameraRay.Direction = dir;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;
    rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, cameraRay);

    while(rayQuery.Proceed())
    {
        if(rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if(TestOpacity(rayQuery.CandidateGeometryIndex(), 
                rayQuery.CandidateInstanceID(), 
                rayQuery.CandidatePrimitiveIndex(), 
                rayQuery.CandidateTriangleBarycentrics()))
            {
                rayQuery.CommitNonOpaqueTriangleHit();
            }
        }
    }

    RayPayload payload;
    payload.t = FLT_MAX;

    if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint meshIdx = rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID();
        uint primIdx = rayQuery.CommittedPrimitiveIndex();
        float2 bary = rayQuery.CommittedTriangleBarycentrics();

        const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(meshIdx)];

        payload.t = rayQuery.CommittedRayT();
        payload.matIdx = meshData.MatIdx;
        payload.hitMeshIdx = meshIdx;

        uint tri = primIdx * 3;
        tri += meshData.BaseIdxOffset;
        uint i0 = g_sceneIndices[NonUniformResourceIndex(tri)] + meshData.BaseVtxOffset;
        uint i1 = g_sceneIndices[NonUniformResourceIndex(tri + 1)] + meshData.BaseVtxOffset;
        uint i2 = g_sceneIndices[NonUniformResourceIndex(tri + 2)] + meshData.BaseVtxOffset;

        Vertex V0 = g_sceneVertices[NonUniformResourceIndex(i0)];
        Vertex V1 = g_sceneVertices[NonUniformResourceIndex(i1)];
        Vertex V2 = g_sceneVertices[NonUniformResourceIndex(i2)];

        float4 q = Math::DecodeSNorm4(meshData.Rotation);
        // due to quantization, it's necessary to renormalize
        q = normalize(q);

        // texture UV coords
        float2 uv = V0.TexUV + bary.x * (V1.TexUV - V0.TexUV) + bary.y * (V2.TexUV - V0.TexUV);
        payload.uv = uv;

        // normal
        float3 v0_n = Math::DecodeOct32(V0.NormalL);
        float3 v1_n = Math::DecodeOct32(V1.NormalL);
        float3 v2_n = Math::DecodeOct32(V2.NormalL);
        float3 normal = v0_n + bary.x * (v1_n - v0_n) + bary.y * (v2_n - v0_n);
        // transform normal using the inverse transpose
        // (M^-1)^T = ((RS)^-1)^T
        //          = (S^-1 R^-1)^T
        //          = (R^T)^T (S^-1)^T
        //          = R S^-1
        const float3 scaleInv = 1.0f / meshData.Scale;
        normal *= scaleInv;
        normal = Math::RotateVector(normal, q);
        normal = normalize(normal);
        payload.normal = normal;

        // tangent vector
        float3 v0_t = Math::DecodeOct32(V0.TangentU);
        float3 v1_t = Math::DecodeOct32(V1.TangentU);
        float3 v2_t = Math::DecodeOct32(V2.TangentU);
        float3 tangent = v0_t + bary.x * (v1_t - v0_t) + bary.y * (v2_t - v0_t);
        tangent *= meshData.Scale;
        tangent = Math::RotateVector(tangent, q);
        tangent = normalize(tangent);
        payload.tangent = tangent;

        // triangle geometry differentials are needed for ray differentials
        float3 v0W = Math::TransformTRS(V0.PosL, meshData.Translation, q, meshData.Scale);
        float3 v1W = Math::TransformTRS(V1.PosL, meshData.Translation, q, meshData.Scale);
        float3 v2W = Math::TransformTRS(V2.PosL, meshData.Translation, q, meshData.Scale);

        float3 n0W = v0_n * scaleInv;
        n0W = Math::RotateVector(n0W, q);
        n0W = normalize(n0W);

        float3 n1W = v1_n * scaleInv;
        n1W = Math::RotateVector(n1W, q);
        n1W = normalize(n1W);

        float3 n2W = v2_n * scaleInv;
        n2W = Math::RotateVector(n2W, q);
        n2W = normalize(n2W);

        Math::TriDifferentials triDiffs = Math::TriDifferentials::Compute(v0W, v1W, v2W, 
            n0W, n1W, n2W,
            V0.TexUV, V1.TexUV, V2.TexUV);

        payload.dpdu = triDiffs.dpdu;
        payload.dpdv = triDiffs.dpdv;
        payload.dndu = triDiffs.dndu;
        payload.dndv = triDiffs.dndv;
        
        // motion vector
        float3 hitPos = mad(rayQuery.WorldRayDirection(), rayQuery.CommittedRayT(), 
            rayQuery.WorldRayOrigin());
        float3 posL = Math::InverseTransformTRS(hitPos, meshData.Translation, q, meshData.Scale);
        float3 prevTranslation = meshData.Translation - meshData.dTranslation;
        float4 q_prev = Math::DecodeSNorm4(meshData.PrevRotation);
        // due to quantization, it's necessary to renormalize
        q_prev = normalize(q_prev);
        float3 pos_prev = Math::TransformTRS(posL, prevTranslation, q_prev, meshData.PrevScale);
        float3 posV_prev = mul(g_frame.PrevView, float4(pos_prev, 1.0f));
        float2 posNDC_prev = posV_prev.xy / (posV_prev.z * g_frame.TanHalfFOV);
        posNDC_prev.x /= g_frame.AspectRatio;
        payload.prevPosNDC = posNDC_prev;
    }

    return payload;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(GBUFFER_RT_GROUP_DIM_X, GBUFFER_RT_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= g_frame.RenderWidth || DTid.y >= g_frame.RenderHeight)
        return;

    float2 lensSample = 0;
    float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
    float3 rayDirCS = RT::GeneratePinholeCameraRay_CS(DTid.xy, renderDim,
        g_frame.AspectRatio, g_frame.TanHalfFOV, g_frame.CurrCameraJitter);
    float3 rayOrigin = g_frame.CameraPos;

    if(g_frame.DoF)
    {
        RNG rng = RNG::Init(RNG::PCG3d(DTid.xyx).zy, g_frame.FrameNum);
        lensSample = Sampling::UniformSampleDiskConcentric(rng.Uniform2D());
        lensSample *= g_frame.LensRadius;
        // Camera space to world space
        rayOrigin += mad(lensSample.x, g_frame.CurrView[0].xyz, lensSample.y * g_frame.CurrView[1].xyz);

#if 0
        // rayDirCS.z = 1
        float t = g_frame.FocusDepth / rayDirCS.z;
        float3 focalPoint = t * rayDirCS;
#else
        float3 focalPoint = g_frame.FocusDepth * rayDirCS;
#endif
        rayDirCS = focalPoint - float3(lensSample, 0);
    }

    // Camera space to world space
    float3 rayDir = mad(rayDirCS.x, g_frame.CurrView[0].xyz, 
        mad(rayDirCS.y, g_frame.CurrView[1].xyz, rayDirCS.z * g_frame.CurrView[2].xyz));
    rayDir = normalize(rayDir);

    RayPayload rayPayload = TracePrimaryHit(rayOrigin, rayDir);

    if(g_local.PickedPixelX == DTid.x && g_local.PickedPixelY == DTid.y)
        g_pick[0] = rayPayload.t != FLT_MAX ? rayPayload.hitMeshIdx : UINT32_MAX;

    // ray missed the scene
    if(rayPayload.t == FLT_MAX)
    {
        RWTexture2D<float> g_depth = ResourceDescriptorHeap[g_local.DepthUavDescHeapIdx];
        g_depth[DTid.xy] = FLT_MAX;

        RWTexture2D<float2> g_metallicRoughness = ResourceDescriptorHeap[g_local.MetallicRoughnessUavDescHeapIdx];
        g_metallicRoughness[DTid.xy].x = 4.0f / 255.0f;

        // just the camera motion
        RWTexture2D<float2> g_outMotion = ResourceDescriptorHeap[g_local.MotionVectorUavDescHeapIdx];
        float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
        float3 motion = g_frame.CameraPos - prevCameraPos;
        float2 motionNDC = motion.z > 0 ? motion.xy / (motion.z * g_frame.TanHalfFOV) : 0;
        motionNDC.x /= g_frame.AspectRatio;
        float2 motionUV = Math::UVFromNDC(motionNDC);
        g_outMotion[DTid.xy] = motionUV;

        return;
    }

    float2 currUV = (DTid.xy + 0.5) / renderDim;
    float2 prevUV = Math::UVFromNDC(rayPayload.prevPosNDC) - (g_frame.CurrCameraJitter / renderDim);
    float2 motionVec = currUV - prevUV;

    // Rate of change of texture uv coords w.r.t. screen space. Needed for texture filtering.
    float4 grads = GBufferRT::UVDifferentials(DTid.xy, rayOrigin, rayDir, g_frame.CurrCameraJitter, 
        g_frame.DoF, lensSample, g_frame.FocusDepth, rayPayload.t, rayPayload.dpdu, 
        rayPayload.dpdv, g_frame);

    // Instead of hit distance, save view z for slighly faster position reconstruction
    float3 pos = mad(rayPayload.t, rayDir, rayOrigin);
    float3 posV = mul(g_frame.CurrView, float4(pos, 1.0f));
    float z = g_frame.DoF ? rayPayload.t : posV.z;
    float3 wo = rayOrigin - pos;

    GBufferRT::ApplyTextureMaps(DTid.xy, z, wo, rayPayload.uv, rayPayload.matIdx, 
        rayPayload.normal, rayPayload.tangent, motionVec, grads, rayPayload.dpdu, 
        rayPayload.dpdv, rayPayload.dndu, rayPayload.dndv, g_frame, 
        g_local, g_materials);
}
