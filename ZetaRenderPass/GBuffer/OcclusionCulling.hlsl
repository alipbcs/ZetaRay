#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "GBuffer_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbOcclussionCulling> g_local : register(b1);
StructuredBuffer<MeshInstance> g_meshes : register(t0);
RWByteAddressBuffer g_indirectDrawArgs : register(u0);

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(OCCLUSION_CULL_THREAD_GROUP_SIZE_X, OCCLUSION_CULL_THREAD_GROUP_SIZE_Y, OCCLUSION_CULL_THREAD_GROUP_SIZE_Z)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= g_local.NumMeshes)
		return;
	
	bool isUnoccluded = true;	
	const uint numUnoccludedInWave = WaveActiveCountBits(isUnoccluded);
	
	// nothing in this wave is unoccluded
	if (!numUnoccludedInWave)
		return;
	
	uint baseIdxToStore;
	
	if (WaveIsFirstLane())
		g_indirectDrawArgs.InterlockedAdd(g_local.CounterBufferOffset, numUnoccludedInWave, baseIdxToStore);

	// broadcast base offset to all lanes
	baseIdxToStore = WaveReadLaneFirst(baseIdxToStore);
	const uint laneIdxToStore = baseIdxToStore + WavePrefixSum(isUnoccluded);
	
	if(isUnoccluded)
	{
		CommandSig cmdSig;
		cmdSig.RootConstant = DTid.x;

		cmdSig.DrawArgs.IndexCountPerInstance = g_meshes[DTid.x].IndexCount;
		cmdSig.DrawArgs.InstanceCount = 1;
		cmdSig.DrawArgs.StartIndexLocation = g_meshes[DTid.x].BaseIdxOffset;
		cmdSig.DrawArgs.BaseVertexLocation = 0;
		cmdSig.DrawArgs.StartInstanceLocation = 0;

		cmdSig.pad0 = 0;
		cmdSig.pad1 = 0;
		
		uint byteOffsetToStore = laneIdxToStore * sizeof(CommandSig);
		g_indirectDrawArgs.Store<CommandSig>(byteOffsetToStore, cmdSig);
	}
}
