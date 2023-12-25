#pragma once

#include "../Math/CollisionTypes.h"
#include "../Core/Vertex.h"
#include "../App/App.h"

namespace ZetaRay::Model
{
    enum class RT_MESH_MODE
    {
        // Slow build time but fastest possible trace time
        STATIC = 0,

        // Dynamic mesh that only needs to update its transform and doesn't need rebuilds
        DYNAMIC_NO_REBUILD,

        // Dynamic mesh that needs rebuild (due to e.g. change in number of vertices or topology)
        //DYNAMIC_REBUILD,
    };

    struct TriangleMesh
    {
        TriangleMesh() = default;
        TriangleMesh(Util::Span<Core::Vertex> vertices,
            uint32_t vtxBuffStartOffset,
            uint32_t idxBuffStartOffset,
            uint32_t numIndices,
            uint32_t matIdx);

        //inline D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const
        //{
        //    D3D12_VERTEX_BUFFER_VIEW vbv;
        //    vbv.BufferLocation = m_vertexBuffer.GetGpuVA();
        //    vbv.SizeInBytes = static_cast<UINT>(m_vertexBuffer.GetDesc().Width);
        //    vbv.StrideInBytes = sizeof(Core::VertexPosNormalTexTangent);

        //    return vbv;
        //}

        //inline D3D12_INDEX_BUFFER_VIEW IndexBufferView() const
        //{
        //    D3D12_INDEX_BUFFER_VIEW ibv;
        //    ibv.BufferLocation = m_indexBuffer.GetGpuVA();
        //    ibv.Format = Core::MESH_INDEX_FORMAT;
        //    ibv.SizeInBytes = static_cast<UINT>(m_indexBuffer.GetDesc().Width);

        //    return ibv;
        //}

        uint32_t m_vtxBuffStartOffset;
        uint32_t m_idxBuffStartOffset;
        uint32_t m_materialIdx;
        uint32_t m_numVertices;
        uint32_t m_numIndices;
        Math::AABB m_AABB;
    };

    // Ref: DirectXTK12 library (MIT License), available from:
    // https://github.com/microsoft/DirectXTK12
    namespace PrimitiveMesh
    {
        //void ComputeBox(Vector<VertexPosNormalTexTangent>& vertices, Vector<uint16_t>& indices, 
        //    Math::float3 size);

        void ComputeSphere(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float diameter, size_t tessellation);
        void ComputeCylinder(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount);
        void ComputeCone(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float diameter, float height, size_t tessellation);
        void ComputeTorus(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float diameter, float thickness, size_t tessellation);
        void ComputeTeapot(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float size, size_t tessellation);
        void ComputeGrid(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float width, float depth, uint32_t m, uint32_t n);
    }
}
