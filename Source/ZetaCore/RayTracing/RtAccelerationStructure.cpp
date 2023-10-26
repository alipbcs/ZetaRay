#include "RtAccelerationStructure.h"
#include "../App/Timer.h"
#include "../Core/RendererCore.h"
#include "../Core/CommandList.h"
#include "../Scene/SceneCore.h"
#include "../Math/MatrixFuncs.h"
#include "../Core/SharedShaderResources.h"
#include "../Math/Color.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RT;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Model;
using namespace ZetaRay::Model::glTF::Asset;

namespace
{
	ZetaInline D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS GetBuildFlagsForRtAS(RT_MESH_MODE t)
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

void StaticBLAS::Rebuild(ComputeCmdList& cmdList)
{
	SceneCore& scene = App::GetScene();

	if (scene.m_numStaticInstances == 0)
		return;

	Assert(scene.m_numOpaqueInstances + scene.m_numNonOpaqueInstances == scene.m_numStaticInstances, "these should match.");

	SmallVector<D3D12_RAYTRACING_GEOMETRY_DESC, App::FrameAllocator> meshDescs;
	meshDescs.resize(scene.m_numStaticInstances);

	constexpr int transfromMatSize = sizeof(BLASTransform);
	const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
	const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();
	const D3D12_GPU_VIRTUAL_ADDRESS transformGpuVa = m_perMeshTransform.GpuVA();
	int currInstance = 0;

	// add a triangle mesh to list of geometries included in BLAS
	// following loops should exactly match the ones in FillMeshTransformBufferForBuild()
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

		for (int i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
		{
			const Scene::RT_Flags flags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[i]);

			if (flags.MeshMode == RT_MESH_MODE::STATIC)
			{
				const uint64_t meshID = currTreeLevel.m_meshIDs[i];
				if (meshID == SceneCore::NULL_MESH)
					continue;

				const TriangleMesh* mesh = scene.GetMesh(meshID).value();

				meshDescs[currInstance].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				// force mesh to be opaque when possible to avoid invoking any-hit shaders
				meshDescs[currInstance].Flags = flags.IsOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
				// elements are tightly packed as size of each element is a multiple of required alignment
				meshDescs[currInstance].Triangles.Transform3x4 = transformGpuVa + currInstance * transfromMatSize;
				meshDescs[currInstance].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
				meshDescs[currInstance].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				meshDescs[currInstance].Triangles.IndexCount = mesh->m_numIndices;
				meshDescs[currInstance].Triangles.VertexCount = mesh->m_numVertices;
				meshDescs[currInstance].Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
				meshDescs[currInstance].Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh->m_vtxBuffStartOffset * sizeof(Vertex);
				meshDescs[currInstance].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

				// refer to notes in TLAS::BuildFrameMeshInstanceData()
				currTreeLevel.m_rtASInfo[i] = RT_AS_Info
				{
					.GeometryIndex = uint32_t(currInstance),
					.InstanceID = 0
				};

				currInstance++;
			}
		}
	}

	Assert((uint32_t)currInstance == scene.m_numStaticInstances, "Invalid number of instances.");

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
	buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	buildDesc.Inputs.Flags = GetBuildFlagsForRtAS(RT_MESH_MODE::STATIC);
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.NumDescs = (UINT)meshDescs.size();
	buildDesc.Inputs.pGeometryDescs = meshDescs.data();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
	App::GetRenderer().GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuild);

	Assert(prebuild.ResultDataMaxSizeInBytes > 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");
	Assert(prebuild.ResultDataMaxSizeInBytes < UINT32_MAX, "Allocation size exceeded maximum allowed.");

	// allocate a new buffer if this is the first time or the old one isn't large enough
	if (!m_buffer.IsInitialized() || m_buffer.Desc().Width < prebuild.ResultDataMaxSizeInBytes)
	{
		m_buffer = GpuMemory::GetDefaultHeapBuffer("StaticBLAS",
			(uint32_t)prebuild.ResultDataMaxSizeInBytes,
			true,
			true);
	}

	// use the same buffer for scratch and compaction info
	m_compactionInfoStartOffset = (uint32_t)Math::AlignUp(prebuild.ScratchDataSizeInBytes,
		alignof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));
	const uint32_t scratchBuffSizeInBytes = m_compactionInfoStartOffset +
		sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);

	m_scratch = GpuMemory::GetDefaultHeapBuffer("StaticBLAS_scratch",
		scratchBuffSizeInBytes,
		D3D12_RESOURCE_STATE_COMMON,
		true);

	// for reading back the compacted size
	m_postBuildInfoReadback = GpuMemory::GetReadbackHeapBuffer(
		sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

	buildDesc.DestAccelerationStructureData = m_buffer.GpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratch.GpuVA();
	buildDesc.SourceAccelerationStructureData = 0;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compactionDesc;
	compactionDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
	compactionDesc.DestBuffer = m_scratch.GpuVA() + m_compactionInfoStartOffset;

	cmdList.PIXBeginEvent("StaticBLASBuild");

	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 1, &compactionDesc);

	// wait until call above is completed before copying the compacted size
	auto barrier = Direct3DUtil::BufferBarrier(m_scratch.Resource(),
		D3D12_BARRIER_SYNC_COMPUTE_SHADING,
		D3D12_BARRIER_SYNC_COPY,
		D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
		D3D12_BARRIER_ACCESS_COPY_SOURCE);

	cmdList.ResourceBarrier(barrier);

	cmdList.CopyBufferRegion(m_postBuildInfoReadback.Resource(),			// dest
		0,																	// dest offset
		m_scratch.Resource(),												// source
		m_compactionInfoStartOffset,										// source offset
		sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC));

	cmdList.PIXEndEvent();

	scene.m_staleStaticInstances = false;
}

void StaticBLAS::FillMeshTransformBufferForBuild()
{
	SceneCore& scene = App::GetScene();

	SmallVector<BLASTransform, App::FrameAllocator> transforms;
	transforms.resize(scene.m_numStaticInstances);

	int currInstance = 0;
	
	// skip the first level
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];

		for (int i = 0; i < currTreeLevel.m_rtFlags.size(); i++)
		{
			if (currTreeLevel.m_meshIDs[i] == SceneCore::NULL_MESH)
				continue;

			const auto rtFlags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[i]);

			if (rtFlags.MeshMode == RT_MESH_MODE::STATIC)
			{
				float4x3& M = currTreeLevel.m_toWorlds[i];

				for (int j = 0; j < 4; j++)
				{
					transforms[currInstance].M[0][j] = M.m[j].x;
					transforms[currInstance].M[1][j] = M.m[j].y;
					transforms[currInstance].M[2][j] = M.m[j].z;
				}

				currInstance++;
			}
		}
	}

	m_perMeshTransform = GpuMemory::GetDefaultHeapBufferAndInit("StaticBLASTransform",
		sizeof(BLASTransform) * scene.m_numStaticInstances, false, transforms.data());
}

void StaticBLAS::DoCompaction(ComputeCmdList& cmdList)
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC compactDesc;

	m_postBuildInfoReadback.Map();
	memcpy(&compactDesc, m_postBuildInfoReadback.MappedMemory(), sizeof(compactDesc));
	m_postBuildInfoReadback.Unmap();

	// scratch buffer is not needed anymore
	m_scratch.Reset();

	Check(compactDesc.CompactedSizeInBytes > 0, "Invalid RtAS compacted size.");

	// allocate a new BLAS with compacted size
	m_bufferCompacted = GpuMemory::GetDefaultHeapBuffer("StaticBLASCompacted",
		(uint32_t)compactDesc.CompactedSizeInBytes,
		true,
		true);

	cmdList.PIXBeginEvent("StaticBLAS::Compaction");

	cmdList.CompactAccelerationStructure(m_bufferCompacted.GpuVA(), m_buffer.GpuVA());

#if 0
	auto barrier = Direct3DUtil::BufferBarrier(m_bufferCompacted.Resource(),
		D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE,
		D3D12_BARRIER_SYNC_RAYTRACING,
		D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
		D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

	cmdList.ResourceBarrier(barrier);
#endif

	cmdList.PIXEndEvent();
}

void StaticBLAS::CompactionCompletedCallback()
{
	m_buffer = ZetaMove(m_bufferCompacted);
	m_postBuildInfoReadback.Reset();
	m_perMeshTransform.Reset();
}

void StaticBLAS::Clear()
{
	m_buffer.Reset();
	m_bufferCompacted.Reset();
	m_perMeshTransform.Reset();
	m_scratch.Reset();
}

//--------------------------------------------------------------------------------------
// DynamicBLAS
//--------------------------------------------------------------------------------------

void DynamicBLAS::Rebuild(ComputeCmdList& cmdList)
{
	SceneCore& scene = App::GetScene();
	const TriangleMesh* mesh = scene.GetMesh(m_meshID).value();

	const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
	const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();

	D3D12_RAYTRACING_GEOMETRY_DESC geoDesc;
	geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	geoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
	geoDesc.Triangles.IndexCount = mesh->m_numIndices;
	geoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	geoDesc.Triangles.Transform3x4 = 0;
	geoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh->m_vtxBuffStartOffset * sizeof(Vertex);
	geoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
	geoDesc.Triangles.VertexCount = mesh->m_numVertices;
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
	m_blasBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS",
		(uint32_t)prebuild.ResultDataMaxSizeInBytes,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		true);

	m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS_scratch",
		(uint32_t)prebuild.ScratchDataSizeInBytes,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		true);

	buildDesc.DestAccelerationStructureData = m_blasBuffer.GpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA();
	buildDesc.SourceAccelerationStructureData = 0;

	cmdList.PIXBeginEvent("DynamicBLASBuild");
	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
	cmdList.PIXEndEvent();

	m_frameBuilt = (uint32_t)App::GetTimer().GetTotalFrameCount();
}

void DynamicBLAS::Update(ComputeCmdList& cmdList)
{
	SceneCore& scene = App::GetScene();
	const TriangleMesh* mesh = scene.GetMesh(m_meshID).value();

	const auto sceneVBGpuVa = scene.GetMeshVB().GpuVA();
	const auto sceneIBGpuVa = scene.GetMeshIB().GpuVA();

	D3D12_RAYTRACING_GEOMETRY_DESC geoDesc;
	geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	geoDesc.Triangles.IndexBuffer = sceneIBGpuVa + mesh->m_idxBuffStartOffset * sizeof(uint32_t);
	geoDesc.Triangles.IndexCount = mesh->m_numIndices;
	geoDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	geoDesc.Triangles.Transform3x4 = 0;
	geoDesc.Triangles.VertexBuffer.StartAddress = sceneVBGpuVa + mesh->m_vtxBuffStartOffset * sizeof(Vertex);
	geoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
	geoDesc.Triangles.VertexCount = mesh->m_numVertices;
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

	if (prebuild.ScratchDataSizeInBytes > m_scratchBuffer.Desc().Width)
	{
		m_scratchBuffer = GpuMemory::GetDefaultHeapBuffer("DynamicBLAS_scratch",
			(uint32_t)prebuild.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			true);
	}

	buildDesc.DestAccelerationStructureData = m_blasBuffer.GpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuffer.GpuVA();
	buildDesc.SourceAccelerationStructureData = m_blasBuffer.GpuVA();

	cmdList.PIXBeginEvent("DynamicBLASUpdate");
	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
	cmdList.PIXEndEvent();
}

void DynamicBLAS::Clear()
{
	m_blasBuffer.Reset();
	m_scratchBuffer.Reset();
}

//--------------------------------------------------------------------------------------
// TLAS
//--------------------------------------------------------------------------------------

void TLAS::Render(CommandList& cmdList)
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	computeCmdList.PIXBeginEvent("TLAS_Build");
	RebuildOrUpdateBLASes(computeCmdList);
	RebuildTLASInstances(computeCmdList);
	RebuildTLAS(computeCmdList);
	computeCmdList.PIXEndEvent();
}

void TLAS::RebuildTLASInstances(ComputeCmdList& cmdList)
{
	SceneCore& scene = App::GetScene();

	const int numInstances = (int)m_dynamicBLASes.size() + (scene.m_numStaticInstances > 0);
	if (numInstances == 0)
		return;

	SmallVector<D3D12_RAYTRACING_INSTANCE_DESC, App::FrameAllocator, 1> tlasInstances;
	tlasInstances.reserve(numInstances);

	if (scene.m_numStaticInstances)
	{
		tlasInstances.push_back(D3D12_RAYTRACING_INSTANCE_DESC{
				.InstanceID = 0,
				.InstanceMask = RT_AS_SUBGROUP::ALL,
				.InstanceContributionToHitGroupIndex = 0,
				.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE,
				.AccelerationStructure = m_staticBLAS.m_buffer.GpuVA()
			});

		// identity transform for static BLAS instance
		auto& instance = tlasInstances.back();

		memset(&instance.Transform, 0, sizeof(BLASTransform));
		instance.Transform[0][0] = 1.0f;
		instance.Transform[1][1] = 1.0f;
		instance.Transform[2][2] = 1.0f;
	}	
	
	int currInstance = (int)tlasInstances.size();
	const int numStaticInstances = scene.m_numStaticInstances;

	// following traversal order must match the one in RebuildOrUpdateBLASes()
	D3D12_RAYTRACING_INSTANCE_DESC instance;

	// skip the first level
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
		const auto& rtFlagVec = currTreeLevel.m_rtFlags;

		// add one TLAS instance for every dynamic mesh
		for (int i = 0; i < rtFlagVec.size(); i++)
		{
			if (currTreeLevel.m_meshIDs[i] == SceneCore::NULL_MESH)
				continue;

			const auto flags = Scene::GetRtFlags(rtFlagVec[i]);

			if (flags.MeshMode != RT_MESH_MODE::STATIC)
			{
				instance.InstanceID = numStaticInstances + Math::Max(currInstance - 1, 0);	// -1 is for not counting static blas instance
				instance.InstanceMask = flags.InstanceMask;
				instance.InstanceContributionToHitGroupIndex = 0;
				instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
				instance.AccelerationStructure = m_dynamicBLASes[Math::Max(currInstance - 1, 0)].m_blasBuffer.GpuVA();

				auto& M = currTreeLevel.m_toWorlds[i];

				for (int j = 0; j < 4; j++)
				{
					instance.Transform[0][j] = M.m[j].x;
					instance.Transform[1][j] = M.m[j].y;
					instance.Transform[2][j] = M.m[j].z;
				}

				currTreeLevel.m_rtASInfo[i].InstanceID = instance.InstanceID;

				tlasInstances[currInstance++] = instance;
			}
		}
	}

	Assert(currInstance == numInstances, "bug");
	const uint32_t sizeInBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances;

	if (!m_tlasInstanceBuff.IsInitialized() || m_tlasInstanceBuff.Desc().Width < sizeInBytes)
	{
		m_tlasInstanceBuff = GpuMemory::GetDefaultHeapBuffer("TLASInstances",
			sizeInBytes, D3D12_RESOURCE_STATE_COMMON, false);
	}

	UploadHeapBuffer scratchBuff = GpuMemory::GetUploadHeapBuffer(sizeInBytes);
	scratchBuff.Copy(0, sizeInBytes, tlasInstances.data());

	cmdList.CopyBufferRegion(m_tlasInstanceBuff.Resource(),
		0,
		scratchBuff.Resource(),
		scratchBuff.Offset(),
		sizeInBytes);

	// wait for copy to be finished before doing compute work
	auto barrier = Direct3DUtil::BufferBarrier(m_tlasInstanceBuff.Resource(),
		D3D12_BARRIER_SYNC_COPY,
		D3D12_BARRIER_SYNC_COMPUTE_SHADING,
		D3D12_BARRIER_ACCESS_COPY_DEST,
		D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

	cmdList.ResourceBarrier(barrier);
}

void TLAS::RebuildTLAS(ComputeCmdList& cmdList)
{
	SceneCore& scene = App::GetScene();

	const int numInstances = (int)m_dynamicBLASes.size() + (scene.m_numStaticInstances > 0);
	if (numInstances == 0)
		return;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
	buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	buildDesc.Inputs.NumDescs = numInstances;
	buildDesc.Inputs.InstanceDescs = m_tlasInstanceBuff.GpuVA();

	auto* device = App::GetRenderer().GetDevice();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
	device->GetRaytracingAccelerationStructurePrebuildInfo(&buildDesc.Inputs, &prebuildInfo);
	Assert(prebuildInfo.ResultDataMaxSizeInBytes != 0, "GetRaytracingAccelerationStructurePrebuildInfo() failed.");

	if (!m_tlasBuffer.IsInitialized() || m_tlasBuffer.Desc().Width < prebuildInfo.ResultDataMaxSizeInBytes)
	{
		// previous TLAS is released with proper fence
		m_tlasBuffer = GpuMemory::GetDefaultHeapBuffer("TLAS",
			(uint32_t)prebuildInfo.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			true);
	}

	if (!m_scratchBuff.IsInitialized() || m_scratchBuff.Desc().Width < prebuildInfo.ScratchDataSizeInBytes)
	{
		m_scratchBuff = GpuMemory::GetDefaultHeapBuffer("TLAS_scratch",
			(uint32_t)prebuildInfo.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_COMMON,
			true);
	}

	buildDesc.DestAccelerationStructureData = m_tlasBuffer.GpuVA();
	buildDesc.ScratchAccelerationStructureData = m_scratchBuff.GpuVA();
	buildDesc.SourceAccelerationStructureData = 0;

	cmdList.BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	// even though TLAS was created with an initial stete of D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
	// the debug layer warns that "D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ and 
	// D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE can only be used with resources 
	// created using D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE or with a legacy InitialState 
	// of D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE".
#if 0
	// wait for build to be finished before doing any raytracing
	D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_tlasBuffer.Resource(),
		D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
		D3D12_BARRIER_SYNC_COMPUTE_SHADING,
		D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
		D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

	cmdList.ResourceBarrier(barrier);
#endif
}

void TLAS::RebuildOrUpdateBLASes(ComputeCmdList& cmdList)
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
	SmallVector<D3D12_BUFFER_BARRIER, App::FrameAllocator> uavBarriers;

	if (scene.m_staleStaticInstances)
	{
		m_staticBLASrebuiltFrame = (uint32_t)App::GetTimer().GetTotalFrameCount();
		m_staticBLAS.Rebuild(cmdList);

		D3D12_BUFFER_BARRIER barrier = Direct3DUtil::BufferBarrier(m_staticBLAS.m_buffer.Resource(),
			D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
			D3D12_BARRIER_SYNC_COMPUTE_SHADING,
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
			D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);

		uavBarriers.push_back(barrier);
	}
	// TODO use a fence instead of assuming the worst case
	else if (m_staticBLASrebuiltFrame == App::GetTimer().GetTotalFrameCount() - Constants::NUM_BACK_BUFFERS)
	{
		m_ready = true;
		m_staticBLAS.DoCompaction(cmdList);
	}
	else if (m_staticBLASrebuiltFrame == App::GetTimer().GetTotalFrameCount() - Constants::NUM_BACK_BUFFERS - 1)
		m_staticBLAS.CompactionCompletedCallback();
	
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
					//uavBarriers.push_back(Direct3DUtil::UAVBarrier(m_dynamicBLASes[idx].m_blasBuffer.Resource()));
				}
				else if (flags.UpdateFlag)
				{
					int idx = FindDynamicBLAS(currTreeLevel.m_IDs[i]);
					Assert(idx != -1, "Instance was set for update, but was never inserted in the TLAS");

					m_dynamicBLASes[idx].Update(cmdList);
					//uavBarriers.push_back(Direct3DUtil::UAVBarrier(m_dynamicBLASes[idx].m_blasBuffer.Resource()));
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

	if (!uavBarriers.empty())
		cmdList.ResourceBarrier(uavBarriers.data(), (uint32_t)uavBarriers.size());
}

void TLAS::Clear()
{
	for (auto& buff : m_dynamicBLASes)
		buff.Clear();

	m_framesMeshInstances.Reset();
	m_tlasBuffer.Reset();
	m_scratchBuff.Reset();
	m_staticBLAS.Clear();
	m_tlasInstanceBuff.Reset();
}

void TLAS::BuildFrameMeshInstanceData()
{
	SceneCore& scene = App::GetScene();
	const uint32_t numInstances = (uint32_t)scene.m_IDtoTreePos.size();
	m_frameInstanceData.resize(numInstances);

	uint32_t currInstance = 0;
	const bool sceneHasEmissives = scene.NumEmissiveInstances() > 0;

	auto addTLASInstance = [&scene, this, &currInstance, sceneHasEmissives](const SceneCore::TreeLevel& currTreeLevel,
		int levelIdx, bool staticMesh)
	{
		if (currTreeLevel.m_meshIDs[levelIdx] == SceneCore::NULL_MESH)
			return;

		const auto rtFlags = Scene::GetRtFlags(currTreeLevel.m_rtFlags[levelIdx]);

		if (staticMesh && rtFlags.MeshMode != RT_MESH_MODE::STATIC)
			return;

		if (!staticMesh && rtFlags.MeshMode == RT_MESH_MODE::STATIC)
			return;

		const TriangleMesh* mesh = scene.GetMesh(currTreeLevel.m_meshIDs[levelIdx]).value();
		const Material* mat = scene.GetMaterial(mesh->m_materialID).value();

		const EmissiveInstance* emissiveInstance = sceneHasEmissives && (rtFlags.InstanceMask & RT_AS_SUBGROUP::EMISSIVE) ?
			scene.m_emissives.FindEmissive(currTreeLevel.m_IDs[levelIdx]).value() :
			nullptr;

		auto& M = currTreeLevel.m_toWorlds[levelIdx];
		v_float4x4 vM = load4x3(M);

		// meshes in TLAS go through the following transformations:
		// 
		// 1. Optional transform during BLAS build
		// 2. Per-instance transform for each BLAS instance in TLAS
		//
		// When accessing triangle data in closest-hit shaders, transform 2 can be accessed
		// using the ObjectToWorld3x4() intrinsic, but transform 1 is lost
		float4a t;
		float4a r;
		float4a s;
		decomposeSRT(vM, s, r, t);

		m_frameInstanceData[currInstance].MatIdx = (uint16_t)mat->GpuBufferIndex();
		m_frameInstanceData[currInstance].BaseVtxOffset = (uint32_t)mesh->m_vtxBuffStartOffset;
		m_frameInstanceData[currInstance].BaseIdxOffset = (uint32_t)mesh->m_idxBuffStartOffset;
		m_frameInstanceData[currInstance].Rotation = snorm4(r);
		m_frameInstanceData[currInstance].Scale = half3(s);
		m_frameInstanceData[currInstance].Translation = float3(t.x, t.y, t.z);
		m_frameInstanceData[currInstance].BaseEmissiveTriOffset = emissiveInstance ? emissiveInstance->BaseTriOffset : -1;
		m_frameInstanceData[currInstance].BaseColorTex = mat->BaseColorTexture == uint32_t(-1) 
			? uint16_t(-1) :
			(uint16_t)mat->BaseColorTexture;

		float alpha = float((mat->BaseColorFactor >> 24) & 0xff) / 255.0f;
		m_frameInstanceData[currInstance].AlphaFactor_Cuttoff = Float2ToRG8(float2(alpha, mat->GetAlphaCuttoff()));

		const float4x3& M_prev = *scene.GetPrevToWorld(currTreeLevel.m_IDs[levelIdx]).value();
		v_float4x4 vM_prev = load4x3(M_prev);
		float4a t_prev;
		float4a r_prev;
		float4a s_prev;
		decomposeSRT(vM_prev, s_prev, r_prev, t_prev);

		m_frameInstanceData[currInstance].PrevRotation = snorm4(r_prev);
		m_frameInstanceData[currInstance].PrevScale = half3(s_prev);
		m_frameInstanceData[currInstance].dTranslation = half3(float3(t.x - t_prev.x, t.y - t_prev.y, t.z - t_prev.z));

		currInstance++;
	};

	// Layout:
	// 
	//  - N static meshes (SM)
	//  - D dynamic meshes (DM)
	//  -------------------------------------------------------------
	// | SM 0 | SM 1 | ... | SM N - 1 | DM 0 | DM 1 | ... | DM D - 1 |
	//  -------------------------------------------------------------
	// 
	// TLAS instance for Static BLAS has instanceID of 0. 
	// TLAS instance for Dynamic BLAS d where 0 <= d < D has InstanceID of N + d
	// With this setup, every instance can use GeometryIndex() + InstanceID() to index into the mesh instance buffer

	const bool rebuildStatic = App::GetScene().m_staleStaticInstances;

	// static meshes
	if (rebuildStatic)
	{
		for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
		{
			auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
			const auto& rtFlagVec = currTreeLevel.m_rtFlags;

			for (int i = 0; i < rtFlagVec.size(); i++)
				addTLASInstance(currTreeLevel, i, true);
		}
	}

	Assert(!rebuildStatic || currInstance == scene.m_numStaticInstances, "bug");
	currInstance = scene.m_numStaticInstances;

	// dynamic meshes
	for (int treeLevelIdx = 1; treeLevelIdx < scene.m_sceneGraph.size(); treeLevelIdx++)
	{
		auto& currTreeLevel = scene.m_sceneGraph[treeLevelIdx];
		const auto& rtFlagVec = currTreeLevel.m_rtFlags;

		for (int i = 0; i < rtFlagVec.size(); i++)
			addTLASInstance(currTreeLevel, i, false);
	}

	const uint32_t sizeInBytes = numInstances * sizeof(RT::MeshInstance);
	auto& renderer = App::GetRenderer();

	if (!m_framesMeshInstances.IsInitialized() || m_framesMeshInstances.Desc().Width < sizeInBytes)
	{
		m_framesMeshInstances = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::RT_FRAME_MESH_INSTANCES,
			sizeInBytes,
			false,
			m_frameInstanceData.data());

		// register the shared resource
		auto& r = App::GetRenderer().GetSharedShaderResources();
		r.InsertOrAssignDefaultHeapBuffer(GlobalResource::RT_FRAME_MESH_INSTANCES, m_framesMeshInstances);
	}
	else
		// this is recorded now but submitted after last frame's submissions
		GpuMemory::UploadToDefaultHeapBuffer(m_framesMeshInstances, sizeInBytes, m_frameInstanceData.data());
}

void TLAS::BuildStaticBLASTransforms()
{
	SceneCore& scene = App::GetScene();

	if (scene.m_staleStaticInstances)
		m_staticBLAS.FillMeshTransformBufferForBuild();
}

int TLAS::FindDynamicBLAS(uint64_t key)
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
