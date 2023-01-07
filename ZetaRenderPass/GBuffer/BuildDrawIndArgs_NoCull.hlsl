#include "../Common/Math.hlsli"
#include "../Common/FrameConstants.h"
#include "GBuffer_Common.h"

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cbOcclussionCulling> g_local : register(b1);
StructuredBuffer<MeshInstance> g_meshes : register(t0);
ByteAddressBuffer g_visibility : register(t4);
RWByteAddressBuffer g_indirectDrawArgs : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool LoadVisibility(uint DTid)
{
	const uint visIdx = g_meshes[DTid].VisibilityIdx;
	const uint byteOffset = (visIdx >> 5) * sizeof(uint);
	const uint visibilityLastFrame = g_visibility.Load(byteOffset);
	return (visibilityLastFrame & (1u << (visIdx & 31))) != 0;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(BUILD_NO_CULL_THREAD_GROUP_SIZE_X, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= g_local.NumMeshes)
		return;
	
	const uint meshBuffIdx = g_local.MeshBufferStartIndex + DTid.x;
	const bool wasVisibleLastFrame = LoadVisibility(meshBuffIdx);
	const uint numVisibleInWave = WaveActiveCountBits(wasVisibleLastFrame);
	
	// nothing in this wave was visible
	if (!numVisibleInWave)
		return;
	
	uint baseIdxToStore;
	
	if (WaveIsFirstLane())
		g_indirectDrawArgs.InterlockedAdd(g_local.CounterBufferOffset, numVisibleInWave, baseIdxToStore);

	// broadcast base index to all lanes
	baseIdxToStore = WaveReadLaneFirst(baseIdxToStore);
	// compaction
	const uint laneIdxToStore = baseIdxToStore + WavePrefixSum(wasVisibleLastFrame);
	
	if (wasVisibleLastFrame)
	{
		CommandSig cmdSig;
		cmdSig.RootConstant = meshBuffIdx; // meshes are partitioned by their PSO

		cmdSig.DrawArgs.IndexCountPerInstance = g_meshes[meshBuffIdx].IndexCount;
		cmdSig.DrawArgs.InstanceCount = 1;
		cmdSig.DrawArgs.StartIndexLocation = g_meshes[meshBuffIdx].BaseIdxOffset;
		cmdSig.DrawArgs.BaseVertexLocation = 0;
		cmdSig.DrawArgs.StartInstanceLocation = 0;

		const uint byteOffsetToStore = g_local.ArgBufferStartOffsetInBytes + laneIdxToStore * sizeof(CommandSig);
		g_indirectDrawArgs.Store<CommandSig>(byteOffsetToStore, cmdSig);
	}
}
