#include "RtAccelerationStructure.h"
#include "../App/Timer.h"
#include "../Core/RendererCore.h"
#include "../Core/CommandList.h"
#include "../Scene/SceneCore.h"
#include "../Math/MatrixFuncs.h"
#include "../Model/Mesh.h"
#include "RtCommon.h"
#include "../Core/SharedShaderResources.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::RT;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Model;

namespace
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS GetBuildFlagsForRtAS(RT_MESH_MODE t)
	{
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS f = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

		if (t == RT_MESH_MODE::STATIC)
		{
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
		}
		else if (t == RT_MESH_MODE::SEMI_DYNAMIC)
		{
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
		}
		else if (t == RT_MESH_MODE::FULL_DYNAMIC)
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

		else if (t == RT_MESH_MODE::PRIMARY)
		{
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
		}

		return f;
	}
}

//--------------------------------------------------------------------------------------
// StaticBLAS
//--------------------------------------------------------------------------------------

void StaticBLAS::Rebuild(ComputeCmdList& cmdList) noexcept
{
	SceneCore& scene = App::GetScene();

	if (scene.m_numStaticInstances == 0)
		return;

	FillMeshTransformBufferForBuild();

	SmallVector<D3D12_RAYTRACING_GEOMETRY_DESC, App::FrameAllocator> mesheDescs;
	mesheDescs.resize(scene.m_numStaticInstances);

	const int transfromMatSize = sizeof(float) * 12;
	int currInstance = 0;

	const auto sceneVBGpuVa = scene.GetMeshVB().GetGpuVA();
	const auto sceneIBGpuVa = scene.GetMeshIB().GetGpuVA();

	// following loop should exactly match the one in FillMeshTransformBufferForBuild()
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		const auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

		for (int i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
		{
			const Scene::RT_Flags flags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[i]);

			if (flags.MeshMode == RT_MESH_MODE::STATIC)
			{
				const uint64_t meshID = currTreeLevel.m_meshIDs[i];
				const auto mesh = scene.GetMesh(meshID);

				mesheDescs[currInstance].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				mesheDescs[currInstance].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
				// elements are tightly packed as size of each element is a multiple of required alignment
				mesheDescs[currInstance].Triangles.Transform3x4 = m_perMeshTransformForBuild.GetGpuVA() + currInstance * transfromMatSize;
				mesheDescs[currInstance].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
				mesheDescs[currInstance].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				mesheDescs[currInstance].Triangles.IndexCount = mesh.m_numIndices;
				mesheDescs[currInstance].Triangles.VertexCount = mesh.m_numVertices;
				mesheDescs[currInstance].Triangles.IndexBuffer = sceneIBGpuVa + mesh.m_idxBuffStartOffset * sizeof(uint32_t);
				mesheDescs[currInstance].Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh.m_vtxBuffStartOffset * sizeof(Vertex);
				mesheDescs[currInstance].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

				currInstance++;

				// clearing the rebuild flag is not actually needed
				// One newly added static instance means the static BLAS need to be rebuilt, so per-instance
				// rebuild flag is not used (Scene sets the "m_staleStaticInstances" flag)
				//Scene::SetRtFlags(RT_MESH_MODE::STATIC, flags.InstanceMask, 0, 0);
			}
		}
	}

	Assert((uint32_t)currInstance == scene.m_numStaticInstances, "Invalid number of instances.");

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
	buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::STATIC);
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.NumDescs = (UINT)mesheDescs.size();
	buildDesc.Inputs.pGeometryDescs = mesheDescs.data();

	auto& renderer = App::GetRenderer();
	auto* device = renderer.GetDevice();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
	device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuild);

	Assert(prebuild.ResultDataMaxSizeInBytes > 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

	// allocate a new buffer only if this is the first time or the old one isn't large enough
	if (!m_blasBuffer.IsInitialized() || m_blasBuffer.GetDesc().Width < prebuild.ResultDataMaxSizeInBytes)
	{
		m_blasBuffer = renderer.GetGpuMemory().GetDefaultHeapBuffer("StaticBLAS",
			prebuild.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			true);
	}

	m_scratchBuffer = renderer.GetGpuMemory().GetDefaultHeapBuffer("StaticBLAS_scratch",
		prebuild.ScratchDataSizeInBytes,
		D3D12_RESOURCE_STATE_COMMON,
		true);

	buildDesc.DestAccelerationStructureData = m_blasBuffer.GetGpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GetGpuVA();
	buildDesc.SourceAccelerationStructureData = 0;

	// compaction
	m_postBuildInfo = renderer.GetGpuMemory().GetDefaultHeapBuffer("StaticBLAS_PostBuild",
		sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC),
		D3D12_RESOURCE_STATE_COMMON,
		true);

	m_postBuildInfoReadback = renderer.GetGpuMemory().GetReadbackHeapBuffer(
		sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compactionDesc;
	compactionDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
	compactionDesc.DestBuffer = m_postBuildInfo.GetGpuVA();

	cmdList.PIXBeginEvent("StaticBLASBuild");
	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 1, &compactionDesc);
	cmdList.PIXEndEvent();
}

void StaticBLAS::FillMeshTransformBufferForBuild() noexcept
{
	SceneCore& scene = App::GetScene();
	const int transfromMatSize = sizeof(float) * 12;

	m_perMeshTransformForBuild = App::GetRenderer().GetGpuMemory().GetUploadHeapBuffer(
		transfromMatSize * scene.m_numStaticInstances, D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT);

	int currInstance = 0;

	// skip the first level
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
		float t[3][4];

		for (int i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
		{
			uint8_t rtFlag = currTreeLevel.m_rtFlags[i];

			if (Scene::GetRtFlags(rtFlag).MeshMode == RT_MESH_MODE::STATIC)
			{
				float4x3& M = currTreeLevel.m_toWorlds[i];

				for (int j = 0; j < 4; j++)
				{
					t[0][j] = M.m[j].x;
					t[1][j] = M.m[j].y;
					t[2][j] = M.m[j].z;
				}

				m_perMeshTransformForBuild.Copy(transfromMatSize * currInstance++, transfromMatSize, t);
			}
		}
	}
}

void StaticBLAS::CopyCompactionSize(ComputeCmdList& cmdList) noexcept
{
	cmdList.PIXBeginEvent("StaticBLAS::CopyCompactionSize");

	cmdList.ResourceBarrier(m_postBuildInfo.GetResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);

	cmdList.CopyBufferRegion(m_postBuildInfoReadback.GetResource(),			// dest
		0,																	// dest offset
		m_postBuildInfo.GetResource(),										// source
		0,																	// source offset
		sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));
	
	cmdList.PIXEndEvent();
}

void StaticBLAS::DoCompaction(ComputeCmdList& cmdList) noexcept
{
	// assumes previous copy from m_postBuildInfo to m_postBuildInfoReadback has already completed

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC compactDesc;

	m_postBuildInfoReadback.Map();
	memcpy(&compactDesc, m_postBuildInfoReadback.GetMappedMemory(), sizeof(compactDesc));
	m_postBuildInfoReadback.Unmap();

	Assert(compactDesc.CompactedSizeInBytes > 0, "Invalid compacted size.");

	m_compactedBlasBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBuffer("CompactedStaticBLAS",
		compactDesc.CompactedSizeInBytes,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		true);

	cmdList.PIXBeginEvent("StaticBLAS::Compaction");
	cmdList.CompactAccelerationStructure(m_compactedBlasBuffer.GetGpuVA(), m_blasBuffer.GetGpuVA());
	cmdList.PIXEndEvent();
}

void StaticBLAS::CompactionCompletedCallback() noexcept
{
	m_blasBuffer = ZetaMove(m_compactedBlasBuffer);
	m_postBuildInfoReadback.Reset();
	m_postBuildInfo.Reset();
}

void StaticBLAS::Clear() noexcept
{
	m_blasBuffer.Reset();
	m_compactedBlasBuffer.Reset();
	m_perMeshTransformForBuild.Reset();
	m_postBuildInfo.Reset();
	m_scratchBuffer.Reset();
}

//--------------------------------------------------------------------------------------
// DynamicBLAS
//--------------------------------------------------------------------------------------

void DynamicBLAS::Rebuild(ComputeCmdList& cmdList) noexcept
{
	SceneCore& scene = App::GetScene();
	auto mesh = scene.GetMesh(m_meshID);

	const auto sceneVBGpuVa = scene.GetMeshVB().GetGpuVA();
	const auto sceneIBGpuVa = scene.GetMeshIB().GetGpuVA();

	D3D12_RAYTRACING_GEOMETRY_DESC geoDesc;
	geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	geoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh.m_idxBuffStartOffset * sizeof(uint32_t);
	geoDesc.Triangles.IndexCount = mesh.m_numIndices;
	geoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	geoDesc.Triangles.Transform3x4 = 0;
	geoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh.m_vtxBuffStartOffset * sizeof(Vertex);
	geoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
	geoDesc.Triangles.VertexCount = mesh.m_numVertices;
	geoDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
	buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::SEMI_DYNAMIC);
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.NumDescs = 1;
	buildDesc.Inputs.pGeometryDescs = &geoDesc;

	auto* device = App::GetRenderer().GetDevice();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
	device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuild);

	Assert(prebuild.ResultDataMaxSizeInBytes > 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

	// TODO Rt memory manager for suballocating BLASes, scratch memoeries, etc.
	m_blasBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBuffer("DynamicBLAS",
		prebuild.ResultDataMaxSizeInBytes,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		true);

	m_scratchBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBuffer("DynamicBLAS_scratch",
		prebuild.ScratchDataSizeInBytes,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		true);

	buildDesc.DestAccelerationStructureData = m_blasBuffer.GetGpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GetGpuVA();
	buildDesc.SourceAccelerationStructureData = 0;

	cmdList.PIXBeginEvent("DynamicBLASBuild");
	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
	cmdList.PIXEndEvent();

	m_frameBuilt = (uint32_t)App::GetTimer().GetTotalFrameCount();
}

void DynamicBLAS::Update(ComputeCmdList& cmdList) noexcept
{
	SceneCore& scene = App::GetScene();
	auto mesh = scene.GetMesh(m_meshID);

	const auto sceneVBGpuVa = scene.GetMeshVB().GetGpuVA();
	const auto sceneIBGpuVa = scene.GetMeshIB().GetGpuVA();

	D3D12_RAYTRACING_GEOMETRY_DESC geoDesc;
	geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	geoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh.m_idxBuffStartOffset * sizeof(uint32_t);
	geoDesc.Triangles.IndexCount = mesh.m_numIndices;
	geoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	geoDesc.Triangles.Transform3x4 = 0;
	geoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh.m_vtxBuffStartOffset * sizeof(Vertex);
	geoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
	geoDesc.Triangles.VertexCount = mesh.m_numVertices;
	geoDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
	buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::SEMI_DYNAMIC);
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.NumDescs = 1;
	buildDesc.Inputs.pGeometryDescs = &geoDesc;

	buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

	auto* device = App::GetRenderer().GetDevice();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
	device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuild);

	Assert(prebuild.ResultDataMaxSizeInBytes > 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

	if (prebuild.ScratchDataSizeInBytes > m_scratchBuffer.GetDesc().Width)
	{
		m_scratchBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBuffer("DynamicBLAS_scratch",
			prebuild.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			true);
	}

	buildDesc.DestAccelerationStructureData = m_blasBuffer.GetGpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GetGpuVA();
	buildDesc.SourceAccelerationStructureData = m_blasBuffer.GetGpuVA();

	cmdList.PIXBeginEvent("DynamicBLASUpdate");
	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
	cmdList.PIXEndEvent();
}

void DynamicBLAS::Clear() noexcept
{
	m_blasBuffer.Reset();
	m_scratchBuffer.Reset();
}

//--------------------------------------------------------------------------------------
// TLAS
//--------------------------------------------------------------------------------------

void TLAS::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	computeCmdList.PIXBeginEvent("TLAS_Build");
	RebuildOrUpdateBLASes(computeCmdList);
	RebuildTLAS(computeCmdList);
	computeCmdList.PIXEndEvent();
}

void TLAS::RebuildTLAS(ComputeCmdList& cmdList) noexcept
{
	SceneCore& scene = App::GetScene();

	const int numInstances = (int)m_dynamicBLASes.size() + (scene.m_numStaticInstances > 0);
	if (numInstances == 0)
		return; 

	m_tlasInstanceBuff = App::GetRenderer().GetGpuMemory().GetUploadHeapBuffer(
		sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances,
		D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);

	D3D12_RAYTRACING_INSTANCE_DESC instance;
	instance.InstanceID = 0;
	instance.InstanceMask = RT_AS_SUBGROUP::ALL;
	instance.InstanceContributionToHitGroupIndex = 0;
	instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;		// force all the meshes to be opaque for now
	instance.AccelerationStructure = m_staticBLAS.m_blasBuffer.GetGpuVA();

	int currInstance = 0;

	// identity transform for BLAS instance
	if(scene.m_numStaticInstances)
	{
		memset(&instance.Transform, 0, 12 * sizeof(float));
		instance.Transform[0][0] = 1.0f;
		instance.Transform[1][1] = 1.0f;
		instance.Transform[2][2] = 1.0f;

		m_tlasInstanceBuff.Copy(currInstance++, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), &instance);
	}

	const int numStaticInstances = scene.m_numStaticInstances;

	// following traversal order must match the one in RebuildOrUpdateBLASes()

	// skip the first level
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		const auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
		const auto& rtFlagVec = currTreeLevel.m_rtFlags;

		// add one TLAS instance for every dynamic mesh
		for (int i = 0; i < rtFlagVec.size(); i++)
		{
			const auto flags = Scene::GetRtFlags(rtFlagVec[i]);

			if (flags.MeshMode != RT_MESH_MODE::STATIC)
			{
				instance.InstanceID = numStaticInstances + Math::Max(currInstance - 1, 0);	// -1 is for not counting static blas instance
				instance.InstanceMask = flags.InstanceMask;
				instance.InstanceContributionToHitGroupIndex = 0;
				// force all the meshes to be opaque for now
				instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;		
				instance.AccelerationStructure = m_dynamicBLASes[Math::Max(currInstance - 1, 0)].m_blasBuffer.GetGpuVA();

				auto& M = currTreeLevel.m_toWorlds[i];

				for (int j = 0; j < 4; j++)
				{
					instance.Transform[0][j] = M.m[j].x;
					instance.Transform[1][j] = M.m[j].y;
					instance.Transform[2][j] = M.m[j].z;
				}

				m_tlasInstanceBuff.Copy(currInstance++ * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), sizeof(instance), &instance);
			}
		}
	}

	Assert(currInstance == numInstances, "bug");

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
	buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.NumDescs = numInstances;
	buildDesc.Inputs.InstanceDescs = m_tlasInstanceBuff.GetGpuVA();

	auto* device = App::GetRenderer().GetDevice();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
	device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuildInfo);
	Assert(prebuildInfo.ResultDataMaxSizeInBytes != 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

	if (!m_tlasBuffer.IsInitialized() || m_tlasBuffer.GetDesc().Width < prebuildInfo.ResultDataMaxSizeInBytes)
	{
		// previous TLAS is released automatically with proper fence
		m_tlasBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBuffer("TLAS",
			prebuildInfo.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			true);
	}

	if (!m_scratchBuff.IsInitialized() || m_scratchBuff.GetDesc().Width < prebuildInfo.ScratchDataSizeInBytes)
	{
		m_scratchBuff = App::GetRenderer().GetGpuMemory().GetDefaultHeapBuffer("TLAS_scratch",
			prebuildInfo.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);
	}

	buildDesc.DestAccelerationStructureData = m_tlasBuffer.GetGpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuff.GetGpuVA();
	buildDesc.SourceAccelerationStructureData = 0;

	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
}

void TLAS::RebuildOrUpdateBLASes(ComputeCmdList& cmdList) noexcept
{
	SceneCore& scene = App::GetScene();

	// From DXR specs:
	// acceleration structures must always be in D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
	// so resource state transitions can’t be used to synchronize between writes and reads of acceleration 
	// structure data. Instead, UAV barriers must be used on resources holding acceleration structure data 
	// between operations that write to an acceleration structure (such as BuildRaytracingAccelerationStructure()) 
	// and operations that read from them (such as DispatchRays())

	// From Ray Tracing Gems Chapter 19:
	// "One important optimization is to ensure that any resource transition barriers that
	// are needed after BLAS updates are deferred to be executed right before the
	// TLAS build, instead of executing these right after each BLAS update. Deferral is
	// important because each of these transition barriers is a synchronization step on
	// the GPU. Having the transitions coalesced into a single point in the command buffer
	// avoids redundant synchronization that would otherwise cause the GPU to frequently
	// become idle."
	SmallVector<D3D12_RESOURCE_BARRIER, App::FrameAllocator> uavBarriers;

	if (scene.m_staleStaticInstances)
	{
		m_staticBLASrebuiltFrame = (uint32_t)App::GetTimer().GetTotalFrameCount();

		m_staticBLAS.Rebuild(cmdList);
		uavBarriers.push_back(Direct3DHelper::UAVBarrier(m_staticBLAS.m_blasBuffer.GetResource()));

		// no need, there's a transition barrier
		//uavBarriers.push_back(Direct3DHelper::UAVBarrier(m_staticBLAS.m_postBuildInfo.GetResource()));
	}
	// assuming rebuild was issued in frame F, issue a compaction command in frame F + 1.
	// In this way, we're guranteed that previous Build and Compaction size queries are finished
	else if(m_staticBLASrebuiltFrame == App::GetTimer().GetTotalFrameCount() - 3)
	{
		m_staticBLAS.DoCompaction(cmdList);
		App::GetRenderer().GetGpuMemory().ReleaseDefaultHeapBuffer(ZetaMove(m_staticBLAS.m_scratchBuffer));
	}
	// compaction command was submitted in the last frame
	else if (m_staticBLASrebuiltFrame == App::GetTimer().GetTotalFrameCount() - 4)
	{
		m_staticBLAS.CompactionCompletedCallback();
	}
	
	// do a sort only if there's been one or more insertions
	bool needsSort = false;

	// skip the first level
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		const auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
		const auto& rtFlagVec = currTreeLevel.m_rtFlags;

		// check if any of the dynamic instances needs to be rebuilt or updated
		for (int i = 0; i < rtFlagVec.size(); i++)
		{
			const Scene::RT_Flags flags = Scene::GetRtFlags(rtFlagVec[i]);
			Assert((flags.RebuildFlag & flags.UpdateFlag) == 0, "Rebuild & update flags can't be set at the same time.");

			if (flags.MeshMode != RT_MESH_MODE::STATIC)
			{
				if (flags.RebuildFlag)
				{
					int idx = FindDynamicBLAS(currTreeLevel.m_IDs[i]);

					// this instance was encountered for the first time. Scene must've set the build flag in this scenario
					if (idx == -1)
					{
						m_dynamicBLASes.emplace_back(currTreeLevel.m_IDs[i], currTreeLevel.m_meshIDs[i]);
						needsSort = true;
						idx = (int)m_dynamicBLASes.size() - 1;
					}
					
					m_dynamicBLASes[idx].Rebuild(cmdList);
					uavBarriers.push_back(Direct3DHelper::UAVBarrier(m_dynamicBLASes[idx].m_blasBuffer.GetResource()));
				}
				else if (flags.UpdateFlag)
				{
					int idx = FindDynamicBLAS(currTreeLevel.m_IDs[i]);
					Assert(idx != -1, "Instance was set for update, but was never inserted in the TLAS");

					m_dynamicBLASes[idx].Update(cmdList);
					uavBarriers.push_back(Direct3DHelper::UAVBarrier(m_dynamicBLASes[idx].m_blasBuffer.GetResource()));
				}
			}
		}
	}

	// TODO fix
	if (needsSort)
	{
		//std::sort(m_dynamicBLASes.begin(), m_dynamicBLASes.end(),
		//	[](const DynamicBLAS& b1, const DynamicBLAS& b2)
		//	{
		//		return b1.m_instanceID < b2.m_instanceID;
		//	});
	}

	if(!uavBarriers.empty())
		cmdList.UAVBarrier((UINT)uavBarriers.size(), uavBarriers.begin());

	// delay the resource transiton up until all the other build/update commands have been recorded
	if (scene.m_staleStaticInstances)
	{
		m_staticBLAS.CopyCompactionSize(cmdList);
		scene.m_staleStaticInstances = false;
	}
}

void TLAS::Clear() noexcept
{
	for (auto& buff : m_dynamicBLASes)
		buff.Clear();

	m_framesMeshInstances.Reset();
	m_tlasBuffer.Reset();
	m_scratchBuff.Reset();
	m_staticBLAS.Clear();
	m_tlasInstanceBuff.Reset();
}

void TLAS::BuildFrameMeshInstanceData() noexcept
{
	SceneCore& scene = App::GetScene();
	const size_t numInstances = scene.m_IDtoTreePos.size();
	SmallVector<RT::MeshInstance, App::FrameAllocator> frameInstanceData;
	frameInstanceData.resize(numInstances);

	uint32_t currInstance = 0;

	auto addTLASInstance = [&frameInstanceData, &currInstance](const TriangleMesh& mesh, const Material& mat, float4x3& M) noexcept
	{
		v_float4x4 vM = load(M);

		// meshes in TLAS go through following transformations:
		// 
		// 1. Optional transform during BLAS build
		// 2. Per-instance transform for each BLAS instance in TLAS
		//
		// When accessing triangle data in closest hit shaders, 2nd transform can be accessed
		// using the ObjectToWorld3x4() intrinsic, but the 1st transform is lost
		float4a t;
		float4a r;
		float4a s;
		decomposeSRT(vM, s, r, t);

		RT::MeshInstance instance;
		instance.MatID = (uint16_t)mat.GpuBufferIndex();
		instance.BaseVtxOffset = (uint32_t)mesh.m_vtxBuffStartOffset;
		instance.BaseIdxOffset = (uint32_t)mesh.m_idxBuffStartOffset;
		instance.Rotation = float4(r.x, r.y, r.z, r.w);
		instance.Scale = half3(s.x, s.y, s.z);

		frameInstanceData[currInstance++] = instance;
	};

	// skip the first level
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
		const auto& rtFlagVec = currTreeLevel.m_rtFlags;

		// Layout:
		//  -----------------------------------------------------------------------------------------------------
		// | static mesh 0 | static mesh 1 | ... | static mesh S - 1 | dynamic mesh 0 | ... | dynamic mesh D - 1 |
		//  -----------------------------------------------------------------------------------------------------
		// TLAS instance for Static BLAS has instance-ID of 0. 
		// TLAS instance for Dynamic BLAS d where 0 <= d < D has Instance-ID of S + d
		// With this setup, every instance can use GeometryIndex() + InstanceID() to index into the mesh instance buffer

		// static meshes
		for (int i = 0; i < rtFlagVec.size(); i++)
		{
			const auto mesh = scene.GetMesh(currTreeLevel.m_meshIDs[i]);
			const auto mat = scene.GetMaterial(mesh.m_materialID);
			auto& M = currTreeLevel.m_toWorlds[i];

			if (Scene::GetRtFlags(rtFlagVec[i]).MeshMode == RT_MESH_MODE::STATIC)
				addTLASInstance(mesh, mat, M);
		}

		// dynamic meshes
		for (int i = 0; i < rtFlagVec.size(); i++)
		{
			const auto mesh = scene.GetMesh(currTreeLevel.m_meshIDs[i]);
			const auto mat = scene.GetMaterial(mesh.m_materialID);
			auto& M = currTreeLevel.m_toWorlds[i];

			if (Scene::GetRtFlags(rtFlagVec[i]).MeshMode != RT_MESH_MODE::STATIC)
				addTLASInstance(mesh, mat, M);
		}
	}

	Assert(currInstance == numInstances, "bug");

	const size_t sizeInBytes = numInstances * sizeof(RT::MeshInstance);
	auto& renderer = App::GetRenderer();

	if (!m_framesMeshInstances.IsInitialized() || m_framesMeshInstances.GetDesc().Width < sizeInBytes)
	{
		m_framesMeshInstances = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(GlobalResource::RT_FRAME_MESH_INSTANCES,
			sizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			false,
			frameInstanceData.data());
	}
	else
		// this is recorded now but submitted after last frame's submissions
		renderer.GetGpuMemory().UploadToDefaultHeapBuffer(m_framesMeshInstances, sizeInBytes, frameInstanceData.data());

	// register the shared resources
	auto& r = App::GetRenderer().GetSharedShaderResources();
	r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES, m_framesMeshInstances);
}

int TLAS::FindDynamicBLAS(uint64_t key) noexcept
{
	int beg = 0;
	int end = (int)m_dynamicBLASes.size();
	int mid = (int)m_dynamicBLASes.size() >> 1;

	while (true)
	{
		if (end - beg <= 2)
			break;

		if (m_dynamicBLASes[mid].m_instanceID < key)
			beg = mid + 1;
		else
			end = mid + 1;

		mid = beg + ((end - beg) >> 1);
	}

	if (m_dynamicBLASes[beg].m_instanceID == key)
		return beg;
	else if (m_dynamicBLASes[mid].m_instanceID == key)
		return mid;

	return -1;
}

