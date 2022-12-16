#pragma once

#include "../App/ZetaRay.h"

namespace ZetaRay::Support
{
	struct TaskSet;
}

namespace ZetaRay::Scene::GlobalResource
{
	inline static constexpr const char* MATERIAL_BUFFER = "MaterailBuffer";
	inline static constexpr const char* BASE_COLOR_DESCRIPTOR_TABLE = "BaseColorDescTable";
	inline static constexpr const char* NORMAL_DESCRIPTOR_TABLE = "NormalDescTable";
	inline static constexpr const char* METALNESS_ROUGHNESS_DESCRIPTOR_TABLE = "MRDescTable";
	inline static constexpr const char* EMISSIVE_DESCRIPTOR_TABLE = "EmissiveDescTable";
	inline static constexpr const char* FRAME_CONSTANTS_BUFFER_NAME = "FrameConstants";
	//inline static constexpr const char* ANALYTICAL_LIGHTS_BUFFER_NAME = "AnalyticalLightsBuffer";
	//inline static constexpr const char* ANALYTICAL_LIGHTS_ALIAS_TABLE_BUFFER_NAME = "LightManager/AnalyticalLightsAliasTable";
	//inline static constexpr const char* EMISSIVE_TRIANGLES_BUFFER_NAME = "LightManager/EmissiveTrianglesBuffer";
	//inline static constexpr const char* EMISSIVE_TRIANGLES_ALIAS_TABLE_BUFFER_NAME = "LightManager/EmissiveTrianglesAliasTable";
	inline static constexpr const char* RT_SCENE_BVH = "RayTracer/SceneBVH";
	inline static constexpr const char* SCENE_VERTEX_BUFFER = "SceneVB";
	inline static constexpr const char* SCENE_INDEX_BUFFER = "SceneIB";
	inline static constexpr const char* RT_FRAME_MESH_INSTANCES = "RtFrameMeshInstances";
}

namespace ZetaRay::Scene::Renderer
{
	using fp_Init = void(*)() noexcept;
	using fp_Update = void(*)(Support::TaskSet& ts) noexcept;
	using fp_Render = void(*)(Support::TaskSet& ts) noexcept;
	using fp_Shutdown = void(*)() noexcept;
	using fp_OnWindowSizeChanged = void(*)() noexcept;
	using fp_DebugDrawRenderGraph = void(*)() noexcept;

	struct Interface
	{
		fp_Init Init;
		fp_Update Update;
		fp_Render Render;
		fp_Shutdown Shutdown;
		fp_OnWindowSizeChanged OnWindowSizeChanged;
		fp_DebugDrawRenderGraph DebugDrawRenderGraph;
	};
}
