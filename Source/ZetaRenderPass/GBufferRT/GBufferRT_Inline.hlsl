#include "GBufferRT.hlsli"
#include "../Common/Common.hlsli"
#include "../Common/Sampling.hlsli"

#define THREAD_GROUP_SWIZZLING 0

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
};

bool TestOpacity(uint geoIdx, uint instanceID, uint primIdx, float2 bary)
{
    const RT::MeshInstance meshData = g_frameMeshData[NonUniformResourceIndex(geoIdx + instanceID)];

	float2 alphaFactor_cutoff = Math::Color::UnpackRG(meshData.AlphaFactor_Cuttoff);
	if(alphaFactor_cutoff.y == 1.0)
	 	return false;

	float alpha = alphaFactor_cutoff.x;
    
	if(meshData.BaseColorTex != -1)
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

		BASE_COLOR_MAP g_baseCol = ResourceDescriptorHeap[NonUniformResourceIndex(g_frame.BaseColorMapsDescHeapOffset + meshData.BaseColorTex)];
		alpha = g_baseCol.SampleLevel(g_samLinearWrap, uv, 0).a;
	}

	if (alpha < alphaFactor_cutoff.y) 
		return false;

    return true;
}

RayPayload PrimaryHitData(float3 cameraRayDir)
{
    RayDesc cameraRay;
	cameraRay.Origin = g_frame.CameraPos;
	cameraRay.TMin = g_frame.CameraNear;
	cameraRay.TMax = FLT_MAX;
	cameraRay.Direction = cameraRayDir;

	// find primary surface point
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;
	
    rayQuery.TraceRayInline(g_bvh, 
        RAY_FLAG_NONE, 
        RT_AS_SUBGROUP::ALL, 
        cameraRay);

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

        uint tri = primIdx * 3;
        tri += meshData.BaseIdxOffset;
        uint i0 = g_sceneIndices[NonUniformResourceIndex(tri)] + meshData.BaseVtxOffset;
        uint i1 = g_sceneIndices[NonUniformResourceIndex(tri + 1)] + meshData.BaseVtxOffset;
        uint i2 = g_sceneIndices[NonUniformResourceIndex(tri + 2)] + meshData.BaseVtxOffset;

        Vertex V0 = g_sceneVertices[NonUniformResourceIndex(i0)];
        Vertex V1 = g_sceneVertices[NonUniformResourceIndex(i1)];
        Vertex V2 = g_sceneVertices[NonUniformResourceIndex(i2)];

        float4 q = Math::Encoding::DecodeSNorm4(meshData.Rotation);
        // due to quantization, it's necessary to renormalize
        q = normalize(q);

        // texture UV coords
        float2 uv = V0.TexUV + bary.x * (V1.TexUV - V0.TexUV) + bary.y * (V2.TexUV - V0.TexUV);
        payload.uv = uv;

        // normal
        float3 v0_n = Math::Encoding::DecodeOct16(V0.NormalL);
        float3 v1_n = Math::Encoding::DecodeOct16(V1.NormalL);
        float3 v2_n = Math::Encoding::DecodeOct16(V2.NormalL);
        float3 normal = v0_n + bary.x * (v1_n - v0_n) + bary.y * (v2_n - v0_n);
        // transform normal using the inverse transpose
        // (M^-1)^T = ((RS)^-1)^T
        //          = (S^-1 R^-1)^T
        //          = (R^T)^T (S^-1)^T
        //          = R S^-1
        normal *= 1.0f / meshData.Scale;
        normal = Math::Transform::RotateVector(normal, q);
        normal = normalize(normal);
        payload.normal = normal;

        // tangent vector
        float3 v0_t = Math::Encoding::DecodeOct16(V0.TangentU);
        float3 v1_t = Math::Encoding::DecodeOct16(V1.TangentU);
        float3 v2_t = Math::Encoding::DecodeOct16(V2.TangentU);
        float3 tangent = v0_t + bary.x * (v1_t - v0_t) + bary.y * (v2_t - v0_t);
        tangent *= meshData.Scale;
        tangent = Math::Transform::RotateVector(tangent, q);
        tangent = normalize(tangent);
        payload.tangent = tangent;

        // dpdu & dpdv for ray differential calculation
        float3 v0W = GBufferRT::TransformTRS(V0.PosL, meshData.Translation, q, meshData.Scale);
        float3 v1W = GBufferRT::TransformTRS(V1.PosL, meshData.Translation, q, meshData.Scale);
        float3 v2W = GBufferRT::TransformTRS(V2.PosL, meshData.Translation, q, meshData.Scale);

        GBufferRT::Triangle_dpdu_dpdv(v0W, v1W, v2W, V0.TexUV, V1.TexUV, V2.TexUV, payload.dpdu, payload.dpdv);
        
        // motion vector
        float3 hitPos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
        float3 posL = GBufferRT::InverseTransformTRS(hitPos, meshData.Translation, q, meshData.Scale);
        float3 prevTranslation = meshData.Translation - meshData.dTranslation;
        float4 q_prev = Math::Encoding::DecodeSNorm4(meshData.PrevRotation);
        // due to quantization, it's necessary to renormalize
        q_prev = normalize(q_prev);
        float3 posW_prev = GBufferRT::TransformTRS(posL, prevTranslation, q_prev, meshData.PrevScale);
        float3 posV_prev = mul(g_frame.PrevView, float4(posW_prev, 1.0f));
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
void main(uint3 DTid : SV_DispatchThreadID, uint Gidx : SV_GroupIndex, uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING == 1
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
    uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, uint16_t2(GBUFFER_RT_GROUP_DIM_X, GBUFFER_RT_GROUP_DIM_Y),
        g_local.DispatchDimX, GBUFFER_RT_TILE_WIDTH, GBUFFER_RT_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
#else
	const uint2 swizzledDTid = DTid.xy;
#endif

    if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
        return;

    float3 cameraRayDir = RT::GeneratePinholeCameraRay(swizzledDTid, 
		float2(g_frame.RenderWidth, g_frame.RenderHeight),
    	g_frame.AspectRatio, 
		g_frame.TanHalfFOV, 
		g_frame.CurrView[0].xyz, 
		g_frame.CurrView[1].xyz, 
		g_frame.CurrView[2].xyz,
        g_frame.CurrCameraJitter);

    RayPayload rayPayload = PrimaryHitData(cameraRayDir);
	
    // ray missed the scene
    if(rayPayload.t == FLT_MAX)
    {
        RWTexture2D<float> g_depth = ResourceDescriptorHeap[g_local.DepthUavDescHeapIdx];
		g_depth[swizzledDTid] = FLT_MAX;

        // just the camera motion
        RWTexture2D<float2> g_outMotion = ResourceDescriptorHeap[g_local.MotionVectorUavDescHeapIdx];
        float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
        float3 motion = g_frame.CameraPos - prevCameraPos;
        float2 motionNDC = motion.xy / (motion.z * g_frame.TanHalfFOV);
        motionNDC.x /= g_frame.AspectRatio;
        float2 motionUV = Math::Transform::UVFromNDC(motionNDC);
        g_outMotion[swizzledDTid] = motionUV;
        
        return;
    }

    float2 currUV = (swizzledDTid + 0.5 + g_frame.CurrCameraJitter) / float2(g_frame.RenderWidth, g_frame.RenderHeight);
    float2 prevUV = Math::Transform::UVFromNDC(rayPayload.prevPosNDC);
    float2 motionVec = currUV - prevUV;

    // Rate of change of texture uv coords w.r.t. screen space. Needed for texture filtering.
	float4 grads = GBufferRT::UVDifferentials(swizzledDTid, g_frame.CameraPos, cameraRayDir, g_frame.CurrCameraJitter, 
        rayPayload.t, rayPayload.dpdu, rayPayload.dpdv, g_frame);

	float3 posW = g_frame.CameraPos + rayPayload.t * cameraRayDir;
    // save the view-space z, so that position can be reconstructed
    float3 posV = mul(g_frame.CurrView, float4(posW, 1.0f));
    GBufferRT::ApplyTextureMaps(swizzledDTid, posV.z, rayPayload.uv, rayPayload.matIdx, rayPayload.normal, 
        rayPayload.tangent, motionVec, grads, posW, g_frame, g_local, g_materials);
}
