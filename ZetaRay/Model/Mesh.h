#pragma once

#include "../Core/DescriptorHeap.h"
#include "../Math/CollisionTypes.h"
#include "../Core/GpuMemory.h"
#include "../Core/Vertex.h"

namespace ZetaRay::Model
{
	struct TriangleMesh
	{
		TriangleMesh() noexcept = default;

		TriangleMesh(Util::Vector<Core::VertexPosNormalTexTangent>&& vertices, 
			Util::Vector<INDEX_TYPE>&& indices,
			uint64_t matID) noexcept;
		
		TriangleMesh(const TriangleMesh&) = delete;
		TriangleMesh& operator=(const TriangleMesh&) = delete;

		TriangleMesh(TriangleMesh&&) noexcept;
		TriangleMesh& operator=(TriangleMesh&&) noexcept;

		inline D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const
		{
			D3D12_VERTEX_BUFFER_VIEW vbv;
			vbv.BufferLocation = m_vertexBuffer.GetGpuVA();
			vbv.SizeInBytes = static_cast<UINT>(m_vertexBuffer.GetDesc().Width);
			vbv.StrideInBytes = sizeof(Core::VertexPosNormalTexTangent);

			return vbv;
		}

		inline D3D12_INDEX_BUFFER_VIEW IndexBufferView() const
		{
			D3D12_INDEX_BUFFER_VIEW ibv;
			ibv.BufferLocation = m_indexBuffer.GetGpuVA();
			ibv.Format = Core::MESH_INDEX_FORMAT;
			ibv.SizeInBytes = static_cast<UINT>(m_indexBuffer.GetDesc().Width);

			return ibv;
		}

		// Descriptors to VB and IB of this mesh
		Core::DescriptorTable m_descTable;

		Math::AABB m_AABB;

		//SmallVector<VertexPosNormalTexTangent> m_vertices;
		//SmallVector<INDEX_TYPE> m_indices;

		Core::DefaultHeapBuffer m_vertexBuffer;
		Core::DefaultHeapBuffer m_indexBuffer;

		uint64_t m_materialID;
		uint32_t m_numVertices;
		uint32_t m_numIndices;
	};

	// Ref: DirectXTK12 library (MIT License), available from:
	// https://github.com/microsoft/DirectXTK12
	namespace PrimitiveMesh
	{
		//void ComputeBox(Vector<VertexPosNormalTexTangent>& vertices, Vector<uint16_t>& indices, 
		//	Math::float3 size) noexcept;

		void ComputeSphere(Util::Vector<Core::VertexPosNormalTexTangent>& vertices, Util::Vector<INDEX_TYPE>& indices,
			float diameter, size_t tessellation) noexcept;
		void ComputeCylinder(Util::Vector<Core::VertexPosNormalTexTangent>& vertices, Util::Vector<INDEX_TYPE>& indices,
			float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount) noexcept;
		void ComputeCone(Util::Vector<Core::VertexPosNormalTexTangent>& vertices, Util::Vector<INDEX_TYPE>& indices,
			float diameter, float height, size_t tessellation) noexcept;
		void ComputeTorus(Util::Vector<Core::VertexPosNormalTexTangent>& vertices, Util::Vector<INDEX_TYPE>& indices,
			float diameter, float thickness, size_t tessellation) noexcept;
		void ComputeTeapot(Util::Vector<Core::VertexPosNormalTexTangent>& vertices, Util::Vector<INDEX_TYPE>& indices,
			float size, size_t tessellation) noexcept;
		void ComputeGrid(Util::Vector<Core::VertexPosNormalTexTangent>& vertices, Util::Vector<INDEX_TYPE>& indices,
			float width, float depth, uint32_t m, uint32_t n) noexcept;
	}
}
