#pragma once

//#include "../Math/CollisionFuncs.h"
#include "../Core/Vertex.h"
#include "../Utility/Span.h"

namespace ZetaRay::Model
{
    enum class RT_MESH_MODE
    {
        // Slow build time but fastest possible trace time
        STATIC = 0,

        // Dynamic mesh that only needs to update its world transform and doesn't need rebuilds
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
            uint32_t matIdx)
            : m_numVertices((uint32_t)vertices.size()),
            m_numIndices(numIndices),
            m_materialIdx(matIdx),
            m_vtxBuffStartOffset(vtxBuffStartOffset),
            m_idxBuffStartOffset(idxBuffStartOffset)
        {
            Assert(vertices.size() < UINT_MAX, "Number of vertices exceeded maximum allowed.");

            //Math::v_AABB vBox = Math::compueMeshAABB(vertices.data(), offsetof(Core::Vertex, Position),
            //    sizeof(Core::Vertex), m_numVertices);
            //m_AABB = Math::store(vBox);
        }

        uint32_t m_vtxBuffStartOffset;
        uint32_t m_idxBuffStartOffset;
        uint32_t m_materialIdx;
        uint32_t m_numVertices;
        uint32_t m_numIndices;
        //Math::AABB m_AABB;
    };

    static_assert(std::is_trivially_default_constructible_v<TriangleMesh>);

    // Ref: DirectXTK12 library (MIT License), available from:
    // https://github.com/microsoft/DirectXTK12
    namespace PrimitiveMesh
    {
        void ComputeSphere(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, 
            Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float diameter, size_t tessellation);
        void ComputeCylinder(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, 
            Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount);
        void ComputeCone(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, 
            Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float diameter, float height, size_t tessellation);
        void ComputeTorus(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, 
            Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float diameter, float thickness, size_t tessellation);
        void ComputeTeapot(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, 
            Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float size, size_t tessellation);
        void ComputeGrid(Util::Vector<Core::Vertex, Support::SystemAllocator>& vertices, 
            Util::Vector<uint32_t, Support::SystemAllocator>& indices,
            float width, float depth, uint32_t m, uint32_t n);
    }
}
