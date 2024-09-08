#pragma once

#include "../Core/Vertex.h"
#include "../Core/GpuMemory.h"
#include "../Math/Matrix.h"
#include "../App/Filesystem.h"
#include "../Core/Material.h"
#include "../Model/Mesh.h"
#include "../Scene/SceneCommon.h"

namespace ZetaRay::Model::glTF::Asset
{
    struct Mesh
    {
        uint32_t SceneID;
        int glTFMaterialIdx;
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
        int MaterialIdx;
    };

    struct InstanceDesc
    {
        Math::AffineTransformation LocalTransform;
        uint32_t SceneID;
        uint64_t ID;
        uint64_t ParentID;
        int MeshIdx;
        int MeshPrimIdx;
        RT_MESH_MODE RtMeshMode;
        uint8_t RtInstanceMask;
        bool IsOpaque;
    };

    struct MaterialDesc
    {
        static constexpr uint64_t INVALID_PATH = UINT64_MAX;

        MaterialDesc()
            : BaseColorTexPath(INVALID_PATH),
            MetallicRoughnessTexPath(INVALID_PATH),
            NormalTexPath(INVALID_PATH),
            EmissiveTexPath(INVALID_PATH),
            MetallicFactor(1.0f),
            RoughnessFactor(1.0f),
            NormalScale(1.0f),
            AlphaCutoff(0.5f),
            AlphaMode(Material::ALPHA_MODE::OPAQUE_),
            DoubleSided(false),
            ID(Scene::DEFAULT_MATERIAL_ID),
            BaseColorFactor(1.0f, 1.0f, 1.0f, 1.0f),
            EmissiveFactor(0.0f, 0.0f, 0.0f),
            EmissiveStrength(1.0f),
            IOR(DEFAULT_ETA_MAT),
            Transmission(0.0f)
        {}

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

        float AlphaCutoff;
        // Unique ID of each material
        uint32_t ID;
        Material::ALPHA_MODE AlphaMode;
        bool DoubleSided;
    };

    struct alignas(64) DDSImage
    {
        Core::GpuMemory::Texture T;
        uint64_t ID;
    };
}