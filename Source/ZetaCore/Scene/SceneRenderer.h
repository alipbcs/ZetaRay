#pragma once

#include "../App/ZetaRay.h"

namespace ZetaRay::Support
{
    struct TaskSet;
}

namespace ZetaRay::Core
{
    class RenderGraph;
}

namespace ZetaRay::Scene::GlobalResource
{
    inline static constexpr const char* MATERIAL_BUFFER = "MaterialBuffer";
    inline static constexpr const char* BASE_COLOR_DESCRIPTOR_TABLE = "BaseColorDescTable";
    inline static constexpr const char* NORMAL_DESCRIPTOR_TABLE = "NormalDescTable";
    inline static constexpr const char* METALLIC_ROUGHNESS_DESCRIPTOR_TABLE = "MRDescTable";
    inline static constexpr const char* EMISSIVE_DESCRIPTOR_TABLE = "EmissiveDescTable";
    inline static constexpr const char* FRAME_CONSTANTS_BUFFER = "FrameConstants";
    inline static constexpr const char* EMISSIVE_TRIANGLE_BUFFER = "EmissiveTriangles";
    inline static constexpr const char* EMISSIVE_TRIANGLE_ALIAS_TABLE = "EmissiveAliasTable";
    inline static constexpr const char* PRESAMPLED_EMISSIVE_SETS = "PresampledEmissiveTris";
    inline static constexpr const char* LIGHT_VOXEL_GRID = "LVG";
    inline static constexpr const char* RT_SCENE_BVH_PREV = "PrevSceneBVH";
    inline static constexpr const char* RT_SCENE_BVH_CURR = "CurrSceneBVH";
    inline static constexpr const char* SCENE_VERTEX_BUFFER = "SceneVB";
    inline static constexpr const char* SCENE_INDEX_BUFFER = "SceneIB";
    inline static constexpr const char* RT_FRAME_MESH_INSTANCES_PREV = "PrevRtFrameMeshInstances";
    inline static constexpr const char* RT_FRAME_MESH_INSTANCES_CURR = "CurrRtFrameMeshInstances";
}

namespace ZetaRay::Scene::Renderer
{
    using fp_Init = void(*)();
    using fp_Update = void(*)(Support::TaskSet& ts);
    using fp_Render = void(*)(Support::TaskSet& ts);
    using fp_Shutdown = void(*)();
    using fp_OnWindowSizeChanged = void(*)();
    using fp_GetRenderGraph = Core::RenderGraph*(*)();
    using fp_DebugDrawRenderGraph = void(*)();
    using fp_IsRTASBuilt = bool(*)();
    using fp_SceneModified = void(*)();
    using fp_Pick = void(*)(uint16 screenPosX, uint16 screenPosY);
    using fp_ClearPick = void(*)();
    using fp_CaptureScreen = void(*)();
    using fp_ToggleEmissives = void(*)();

    struct Interface
    {
        fp_Init Init;
        fp_Update Update;
        fp_Render Render;
        fp_Shutdown Shutdown;
        fp_OnWindowSizeChanged OnWindowSizeChanged;
        fp_GetRenderGraph GetRenderGraph;
        fp_DebugDrawRenderGraph DebugDrawRenderGraph;
        fp_IsRTASBuilt IsRTASBuilt;
        fp_SceneModified SceneModified;
        fp_Pick Pick;
        fp_ClearPick ClearPick;
        fp_CaptureScreen CaptureScreen;
        fp_ToggleEmissives ToggleEmissives;
    };
}
