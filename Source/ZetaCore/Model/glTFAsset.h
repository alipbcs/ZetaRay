#pragma once

#include "../Core/Vertex.h"
#include "../Core/GpuMemory.h"
#include "../Math/Matrix.h"
#include "../App/Filesystem.h"
#include "../Core/Material.h"
#include "../Model/Mesh.h"

namespace ZetaRay::Model::glTF::Asset
{
    struct Mesh
    {
        int MaterialIdx;
        int MeshIdx;
        int MeshPrimIdx;
        uint32_t BaseVtxOffset;
        uint32_t BaseIdxOffset;
        uint32_t NumVertices;
        uint32_t NumIndices;
    };

    struct EmissiveInstance
    {
        uint64_t InstanceID;
        uint32_t BaseTriOffset;
        uint32_t NumTriangles;
    };

    struct InstanceDesc
    {
        Math::AffineTransformation LocalTransform;
        int MeshIdx;
        uint64_t ID;
        uint64_t ParentID;
        int MeshPrimIdx;
        RT_MESH_MODE RtMeshMode;
        uint8_t RtInstanceMask;
        bool IsOpaque;
    };

    struct MaterialDesc
    {
        MaterialDesc()
            : BaseColorTexPath(uint64_t(-1)),
            MetallicRoughnessTexPath(uint64_t(-1)),
            NormalTexPath(uint64_t(-1)),
            EmissiveTexPath(uint64_t(-1)),
            MetallicFactor(1.0f),
            RoughnessFactor(1.0f),
            NormalScale(1.0f),
            AlphaCuttoff(0.5f),
            AlphaMode(Material::ALPHA_MODE::OPAQUE_),
            DoubleSided(false),
            Index(-1),
            BaseColorFactor(1.0f, 1.0f, 1.0f, 1.0f),
            EmissiveFactor(0.0f, 0.0f, 0.0f),
            EmissiveStrength(1.0f),
            IOR(1.5f),
            Transmission(0.0f)
        {}

        // Unique index of each material within the glTF scene
        int Index;

        uint64_t BaseColorTexPath;
        uint64_t MetallicRoughnessTexPath;
        uint64_t NormalTexPath;
        uint64_t EmissiveTexPath;

        Math::float4 BaseColorFactor;
        Math::float3 EmissiveFactor;
        float EmissiveStrength;
        float MetallicFactor;
        float RoughnessFactor;
        float NormalScale;
        float IOR;
        float Transmission;

        float AlphaCuttoff;
        Material::ALPHA_MODE AlphaMode;
        bool DoubleSided;
    };

    struct alignas(64) DDSImage
    {
        Core::GpuMemory::Texture T;
        uint64_t ID;
    };
}