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

        uint64_t BaseColorTexPath = INVALID_PATH;
        uint64_t MetallicRoughnessTexPath = INVALID_PATH;
        uint64_t NormalTexPath = INVALID_PATH;
        uint64_t EmissiveTexPath = INVALID_PATH;

        // Base
        Math::float4 BaseColorFactor = Math::float4(1.0f);
        float MetallicFactor = 1.0f;
        // Specular
        float SpecularRoughnessFactor = 1.0f;
        float SpecularIOR = DEFAULT_ETA_MAT;
        // Transmission
        float TransmissionWeight = 0.0f;
        Math::float3 TransmissionColor = Math::float3(1.0f);
        float TransmissionDepth = 0.0f;
        // Subsurface
        float SubsurfaceWeight = 0.0f;
        // Coat
        float CoatWeight = 0.0f;
        Math::float3 CoatColor = Math::float3(0.8f);
        float CoatRoughness = 0.0f;
        float CoatIOR = DEFAULT_ETA_COAT;
        // Emission
        float EmissiveStrength = 1.0f;
        Math::float3 EmissiveFactor = Math::float3(0.0f);
        // Geometry
        float NormalScale = 1.0f;
        float AlphaCutoff = 0.5f;
        Material::ALPHA_MODE AlphaMode = Material::ALPHA_MODE::OPAQUE_;
        bool DoubleSided = false;
        // Unique ID of each material
        uint32_t ID = Scene::DEFAULT_MATERIAL_ID;
    };

    struct alignas(64) DDSImage
    {
        Core::GpuMemory::Texture T;
        uint64_t ID;
    };
}