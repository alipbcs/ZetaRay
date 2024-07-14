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

//--------------------------------------------------------------------------------------
// Payload and State Subobjects
//--------------------------------------------------------------------------------------

struct [raypayload] RayPayload
{
    float t : read(caller) : write(closesthit, miss);
    float3 normal : read(caller) : write(closesthit);
    float3 tangent : read(caller) : write(closesthit);
    float2 uv : read(caller) : write(closesthit);
    uint matIdx : read(caller) : write(closesthit);
    float2 prevPosNDC : read(caller) : write(closesthit);
    float3 dpdu : read(caller) : write(closesthit);
    float3 dpdv : read(caller) : write(closesthit);
};

TriangleHitGroup MyHitGroup =
{
    "TestOpacity",          // AnyHit
    "PrimaryHitData"        // ClosestHit
};

RaytracingShaderConfig MyShaderConfig =
{
    sizeof(RayPayload),                             // max payload size
    sizeof(BuiltInTriangleIntersectionAttributes)   // max attribute size
};

RaytracingPipelineConfig1 MyPipelineConfig =
{
    1,                      // max trace recursion depth
    RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES
};

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[shader("raygeneration")]
void Raygen()
{
    float2 lensSample = 0;
    float2 renderDim = DispatchRaysDimensions().xy;
    float3 rayDirCS = RT::GeneratePinholeCameraRay_CS(DispatchRaysIndex().xy, renderDim,
        g_frame.AspectRatio, g_frame.TanHalfFOV, g_frame.CurrCameraJitter);

    if(g_frame.DoF)
    {
        RNG rng = RNG::Init(RNG::PCG3d(DispatchRaysIndex().xyx).zy, g_frame.FrameNum);
        lensSample = Sampling::UniformSampleDiskConcentric(rng.Uniform2D());
        lensSample *= g_frame.LensRadius;

        float t = g_frame.FocusDepth / rayDirCS.z;
        float3 focalPoint = t * rayDirCS;
        rayDirCS = focalPoint - float3(lensSample, 0);
    }

    // Camera space to world space
    float3 rayOrigin = g_frame.CameraPos;
    rayOrigin += mad(lensSample.x, g_frame.CurrView[0].xyz, lensSample.y * g_frame.CurrView[1].xyz);

    float3 rayDir = mad(rayDirCS.x, g_frame.CurrView[0].xyz, 
        mad(rayDirCS.y, g_frame.CurrView[1].xyz, rayDirCS.z * g_frame.CurrView[2].xyz));
    rayDir = normalize(rayDir);

    RayDesc cameraRay;
    cameraRay.Origin = rayOrigin;
    cameraRay.TMin = 0;
    cameraRay.TMax = FLT_MAX;
    cameraRay.Direction = rayDir;

    // find primary surface point
    RayPayload rayPayload;

    TraceRay(g_bvh,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, // RayFlags
        0xff,                                // InstanceInclusionMask
        0,                                   // RayContributionToHitGroupIndex
        0,                                   // MultiplierForGeometryContributionToShaderIndex
        0,                                   // MissShaderIndex
        cameraRay,
        rayPayload);

    // ray missed the scene, just set the depth
    if(rayPayload.t == FLT_MAX)
    {
        RWTexture2D<float> g_depth = ResourceDescriptorHeap[g_local.DepthUavDescHeapIdx];
        g_depth[DispatchRaysIndex().xy] = FLT_MAX;

        RWTexture2D<float2> g_metallicRoughness = ResourceDescriptorHeap[g_local.MetallicRoughnessUavDescHeapIdx];
        g_metallicRoughness[DispatchRaysIndex().xy].x = 4.0f / 255.0f;

        RWTexture2D<float2> g_outMotion = ResourceDescriptorHeap[g_local.MotionVectorUavDescHeapIdx];
        float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
        float3 motion = g_frame.CameraPos - prevCameraPos;
        float2 motionNDC = motion.xy / (motion.z * g_frame.TanHalfFOV);
        motionNDC.x /= g_frame.AspectRatio;
        float2 motionUV = Math::UVFromNDC(motionNDC);
        g_outMotion[DispatchRaysIndex().xy] = motionUV;

        return;
    }

    uint matIdx = rayPayload.matIdx;
    float3 normal = rayPayload.normal;
    float3 tangent = rayPayload.tangent;
    float2 prevUV = Math::UVFromNDC(rayPayload.prevPosNDC);
    float2 currUV = (DispatchRaysIndex().xy + 0.5 + g_frame.CurrCameraJitter) / DispatchRaysDimensions().xy;
    float2 motionVec = currUV - prevUV;

    float4 grads = GBufferRT::UVDifferentials(DispatchRaysIndex().xy, rayOrigin, rayDir, 
        g_frame.CurrCameraJitter, g_frame.DoF, lensSample, g_frame.FocusDepth, rayPayload.t, 
        rayPayload.dpdu, rayPayload.dpdv, g_frame);

    float3 pos = g_frame.CameraPos + rayPayload.t * rayDir;
    float3 posV = mul(g_frame.CurrView, float4(pos, 1.0f));
    GBufferRT::ApplyTextureMaps(DispatchRaysIndex().xy, posV.z, rayPayload.uv, matIdx, normal, tangent, motionVec, 
        grads, pos, g_frame, g_local, g_materials);
}

[shader("anyhit")]
void TestOpacity(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(GeometryIndex() + InstanceID())];
    float2 alphaFactor_cutoff = Math::UnpackRG(meshData.AlphaFactor_Cuttoff);

    if(alphaFactor_cutoff.y == 1.0)
        IgnoreHit();

    float alpha = alphaFactor_cutoff.x;

    if(meshData.BaseColorTex != UINT16_MAX)
    {
        uint tri = PrimitiveIndex() * 3;
        tri += meshData.BaseIdxOffset;
        uint i0 = g_sceneIndices[NonUniformResourceIndex(tri)] + meshData.BaseVtxOffset;
        uint i1 = g_sceneIndices[NonUniformResourceIndex(tri + 1)] + meshData.BaseVtxOffset;
        uint i2 = g_sceneIndices[NonUniformResourceIndex(tri + 2)] + meshData.BaseVtxOffset;

        Vertex V0 = g_sceneVertices[NonUniformResourceIndex(i0)];
        Vertex V1 = g_sceneVertices[NonUniformResourceIndex(i1)];
        Vertex V2 = g_sceneVertices[NonUniformResourceIndex(i2)];

        float2 uv = V0.TexUV + attr.barycentrics.x * (V1.TexUV - V0.TexUV) + attr.barycentrics.y * (V2.TexUV - V0.TexUV);

        BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + meshData.BaseColorTex)];
        alpha *= g_baseCol.SampleLevel(g_samLinearWrap, uv, 0).a;
    }

    if (alpha < alphaFactor_cutoff.y) 
        IgnoreHit();
}

[shader("closesthit")]
void PrimaryHitData(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(GeometryIndex() + InstanceID())];

    payload.t = RayTCurrent();
    payload.matIdx = meshData.MatIdx;

    uint tri = PrimitiveIndex() * 3;
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
    float2 uv = V0.TexUV + attr.barycentrics.x * (V1.TexUV - V0.TexUV) + attr.barycentrics.y * (V2.TexUV - V0.TexUV);
    payload.uv = uv;

    // normal
    float3 v0_n = Math::DecodeOct32(V0.NormalL);
    float3 v1_n = Math::DecodeOct32(V1.NormalL);
    float3 v2_n = Math::DecodeOct32(V2.NormalL);
    float3 normal = v0_n + attr.barycentrics.x * (v1_n - v0_n) + attr.barycentrics.y * (v2_n - v0_n);
    // transform normal using the inverse transpose
    // (M^-1)^T = ((RS)^-1)^T
    //          = (S^-1 R^-1)^T
    //          = (R^T)^T (S^-1)^T
    //          = R S^-1
    normal *= 1.0f / meshData.Scale;
    normal = Math::RotateVector(normal, q);
    normal = normalize(normal);
    payload.normal = normal;

    // tangent vector
    float3 v0_t = Math::DecodeOct32(V0.TangentU);
    float3 v1_t = Math::DecodeOct32(V1.TangentU);
    float3 v2_t = Math::DecodeOct32(V2.TangentU);
    float3 tangent = v0_t + attr.barycentrics.x * (v1_t - v0_t) + attr.barycentrics.y * (v2_t - v0_t);
    tangent *= meshData.Scale;
    tangent = Math::RotateVector(tangent, q);
    tangent = normalize(tangent);
    payload.tangent = tangent;

    float3 v0W = GBufferRT::TransformTRS(V0.PosL, meshData.Translation, q, meshData.Scale);
    float3 v1W = GBufferRT::TransformTRS(V1.PosL, meshData.Translation, q, meshData.Scale);
    float3 v2W = GBufferRT::TransformTRS(V2.PosL, meshData.Translation, q, meshData.Scale);

    // motion vector
    float4 q_prev = Math::DecodeSNorm4(meshData.PrevRotation);
    // due to quantization, it's necessary to renormalize
    q_prev = normalize(q_prev);

    float3 pos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 posL = GBufferRT::InverseTransformTRS(pos, meshData.Translation, q, meshData.Scale);
    float3 prevTranslation = meshData.Translation - meshData.dTranslation;
    float3 pos_prev = GBufferRT::TransformTRS(posL, prevTranslation, q_prev, meshData.PrevScale);
    float3 posV_prev = mul(g_frame.PrevView, float4(pos_prev, 1.0f));
    float2 posNDC_prev = posV_prev.xy / (posV_prev.z * g_frame.TanHalfFOV);
    posNDC_prev.x /= g_frame.AspectRatio;
    payload.prevPosNDC = posNDC_prev;

    float3 dpdu, dpdv;
    GBufferRT::Triangle_dpdu_dpdv(v0W, v1W, v2W, V0.TexUV, V1.TexUV, V2.TexUV, dpdu, dpdv);
    payload.dpdu = dpdu;
    payload.dpdv = dpdv;
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.t = FLT_MAX;
}