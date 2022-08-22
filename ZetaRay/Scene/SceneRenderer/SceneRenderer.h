#pragma once

#include "../../Core/RenderGraph.h"
#include "../../RenderPass/Common/LightSourceData.h"

namespace ZetaRay::Win32::Filesystem
{
	struct Path;
}

namespace ZetaRay
{
	struct TaskSet;
	struct ParamVariant;

	class SceneRenderer
	{
	public:
		inline static const char* MATERIAL_BUFFER = "AssetManager/MaterailBuffer";
		inline static const char* BASE_COLOR_DESCRIPTOR_TABLE = "AssetManager/BaseColorDescTable";
		inline static const char* METALNESS_ROUGHNESS_DESCRIPTOR_TABLE = "AssetManager/MRDescTable";
		inline static const char* NORMAL_DESCRIPTOR_TABLE = "AssetManager/NormalDescTable";
		inline static const char* EMISSIVE_DESCRIPTOR_TABLE = "AssetManager/EmissiveDescTable";
		inline static const char* FRAME_CONSTANTS_BUFFER_NAME = "FrameConstants";
//		inline static const char* ANALYTICAL_LIGHTS_BUFFER_NAME = "AnalyticalLightsBuffer";
//		inline static const char* ANALYTICAL_LIGHTS_ALIAS_TABLE_BUFFER_NAME = "LightManager/AnalyticalLightsAliasTable";
		inline static const char* EMISSIVE_TRIANGLES_BUFFER_NAME = "LightManager/EmissiveTrianglesBuffer";
		inline static const char* EMISSIVE_TRIANGLES_ALIAS_TABLE_BUFFER_NAME = "LightManager/EmissiveTrianglesAliasTable";
		inline static const char* ENV_LIGHT_PATCH_BUFFER = "LightManager/EnvLightPatches";
		inline static const char* ENV_MAP_ALIAS_TABLE = "LightManager/EnvLightAliasTable";
		inline static const char* RT_SCENE_BVH = "RayTracer/SceneBVH";		
		inline static const char* FRAME_MESH_INSTANCE_DATA = "RayTracer/FrameMeshInstances";

		SceneRenderer() noexcept;
		~SceneRenderer() noexcept;

		void Init() noexcept;
		void Update(TaskSet& ts) noexcept;
		void Render(TaskSet& ts) noexcept;
		void Shutdown() noexcept;

		//void AddAnalyticalLightSource(LightSourceType t) noexcept;
		void AddEmissiveInstance(uint64_t instanceID, Vector<float, 32>&& lumen) noexcept;
		void SetEnvLightSource(const Win32::Filesystem::Path& pathToEnvLight, const Win32::Filesystem::Path& pathToPatches) noexcept;
		void OnWindowSizeChanged() noexcept;

		void DebugDrawRenderGraph() noexcept { m_renderGraph.DebugDrawGraph(); }
		//RenderGraph& GetRenderGraph() { return m_renderGraph; }

		struct PrivateData;

	private:
		void UpdateFrameConstants() noexcept;
		void RayOffset(const ParamVariant& p) noexcept;
		void SetTAAEnablement(const ParamVariant& p) noexcept;
		void SetIndirectDiffuseEnablement(const ParamVariant& p) noexcept;
		void SetIndierctDiffuseDenoiserEnablement(const ParamVariant& p) noexcept;
		void SetInscatteringEnablement(const ParamVariant& p) noexcept;
		void SetUpscalingMethod(const ParamVariant& p) noexcept;
		void ModifySunDir(const ParamVariant& p) noexcept;
		void ModifySunLux(const ParamVariant& p) noexcept;
		void ModifySunAngularRadius(const ParamVariant& p) noexcept;
		void ModifyRayleighSigmaSColor(const ParamVariant& p) noexcept;
		void ModifyRayleighSigmaSScale(const ParamVariant& p) noexcept;
		void ModifyMieSigmaA(const ParamVariant& p) noexcept;
		void ModifyMieSigmaS(const ParamVariant& p) noexcept;
		void ModifyOzoneSigmaAColor(const ParamVariant& p) noexcept;
		void ModifyOzoneSigmaAScale(const ParamVariant& p) noexcept;
		void ModifygForPhaseHG(const ParamVariant& p) noexcept;

		std::unique_ptr<PrivateData> m_data;
		RenderGraph m_renderGraph;
		DefaultHeapBuffer m_frameConstantsBuff;

		struct Defaults
		{		
			// Ref: "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020
			inline static constexpr Math::float3 SIGMA_S_RAYLEIGH = Math::float3(5.802f, 13.558f, 33.1f) * 1e-3f;	// 1 / km
			inline static constexpr float SIGMA_S_MIE = 3.996f * 1e-3f;		// Mie scattering is not wavelength-dependant
			inline static constexpr float SIGMA_A_MIE = 4.4f * 1e-3f;
			inline static constexpr Math::float3 SIGMA_A_OZONE = Math::float3(0.65f, 1.881f, 0.085f) * 1e-3f;
			static constexpr float g = 0.8f;
			static constexpr float ATMOSPHERE_ALTITUDE = 100.0f;		// km
			static constexpr float PLANET_RADIUS = 6360.0f;				// km
			static constexpr float RAY_T_OFFSET = 5e-3;
		};

		enum Upscaling
		{
			POINT,
			FSR2,
			NATIVE,
			COUNT
		};

		inline static const char* UpscalingOptions[] = { "Point", "FSR 2.0 (Quality)", "Native" };

	};
}