#pragma once

#include "../Math/CollisionTypes.h"
#include "../Core/Vertex.h"
#include "../App/App.h"

namespace ZetaRay::Model
{
	// Ref: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
	// This information helps in deciding which acceleration structure flags
	// to use and how or whether to group models together in one BLAS
	enum class RT_MESH_MODE
	{
		// slow build time but fastest possible trace time
		STATIC = 0,

		// dynamic meshes that don't change drastically, (change in number of
		// primitives constituting the mesh, fast-moving objects, ...), can be updated and is
		// fast to rebuild
		SEMI_DYNAMIC,

		// dynamic mesh for which rebuilding is more efficient (w.r.t. acceleration
		// structure quality) than updating due to their dynamic behavior
		FULL_DYNAMIC,

		// mesh that potentially many rays would hit, fastest trace and can be updated
		PRIMARY
	};

	struct TriangleMesh
	{
		TriangleMesh() noexcept = default;

		TriangleMesh(Util::Span<Core::Vertex> vertices,
			size_t vtxBuffStartOffset,
			size_t idxBuffStartOffset,
			uint32_t numIndices,
			uint64_t matID) noexcept;
		
		//inline D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const
		//{
		//	D3D12_VERTEX_BUFFER_VIEW vbv;
		//	vbv.BufferLocation = m_vertexBuffer.GetGpuVA();
		//	vbv.SizeInBytes = static_cast<UINT>(m_vertexBuffer.GetDesc().Width);
		//	vbv.StrideInBytes = sizeof(Core::VertexPosNormalTexTangent);

		//	return vbv;
		//}

		//inline D3D12_INDEX_BUFFER_VIEW IndexBufferView() const
		//{
		//	D3D12_INDEX_BUFFER_VIEW ibv;
		//	ibv.BufferLocation = m_indexBuffer.GetGpuVA();
		//	ibv.Format = Core::MESH_INDEX_FORMAT;
		//	ibv.SizeInBytes = static_cast<UINT>(m_indexBuffer.GetDesc().Width);

		//	return ibv;
		//}

		size_t m_vtxBuffStartOffset;
		size_t m_idxBuffStartOffset;
		uint64_t m_materialID;
		uint32_t m_numVertices;
		uint32_t m_numIndices;
		Math::AABB m_AABB;
	};

	// Ref: DirectXTK12 library (MIT License), available from:
	// https://github.com/microsoft/DirectXTK12
	namespace PrimitiveMesh
	{
		//void ComputeBox(Vector<VertexPosNormalTexTangent>& vertices, Vector<uint16_t>& indices, 
		//	Math::float3 size) noexcept;

		void ComputeSphere(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
			float diameter, size_t tessellation) noexcept;
		void ComputeCylinder(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
			float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount) noexcept;
		void ComputeCone(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
			float diameter, float height, size_t tessellation) noexcept;
		void ComputeTorus(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
			float diameter, float thickness, size_t tessellation) noexcept;
		void ComputeTeapot(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
			float size, size_t tessellation) noexcept;
		void ComputeGrid(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
			float width, float depth, uint32_t m, uint32_t n) noexcept;
	}
}
