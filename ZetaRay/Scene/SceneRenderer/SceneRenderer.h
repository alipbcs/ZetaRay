#pragma once

#include "../../Math/Vector.h"
#include <memory>	// std::unique_ptr

namespace ZetaRay::Support
{
	struct TaskSet;
	struct ParamVariant;
}

namespace ZetaRay::Scene::Render
{
	struct PrivateData;
}

namespace ZetaRay::Scene
{
	class SceneRenderer
	{
	public:
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

		SceneRenderer() noexcept;
		~SceneRenderer() noexcept;

		void Init() noexcept;
		void Update(Support::TaskSet& ts) noexcept;
		void Render(Support::TaskSet& ts) noexcept;
		void Shutdown() noexcept;

		void OnWindowSizeChanged() noexcept;
		void DebugDrawRenderGraph() noexcept;	

	private:
		void RayOffset(const Support::ParamVariant& p) noexcept;
		void SetAA(const Support::ParamVariant& p) noexcept;
		void SetIndirectDiffuseEnablement(const Support::ParamVariant& p) noexcept;
		void SetIndierctDiffuseDenoiser(const Support::ParamVariant& p) noexcept;
		void SetInscatteringEnablement(const Support::ParamVariant& p) noexcept;
		void ModifySunDir(const Support::ParamVariant& p) noexcept;
		void ModifySunLux(const Support::ParamVariant& p) noexcept;
		void ModifySunAngularRadius(const Support::ParamVariant& p) noexcept;
		void ModifyRayleighSigmaSColor(const Support::ParamVariant& p) noexcept;
		void ModifyRayleighSigmaSScale(const Support::ParamVariant& p) noexcept;
		void ModifyMieSigmaA(const Support::ParamVariant& p) noexcept;
		void ModifyMieSigmaS(const Support::ParamVariant& p) noexcept;
		void ModifyOzoneSigmaAColor(const Support::ParamVariant& p) noexcept;
		void ModifyOzoneSigmaAScale(const Support::ParamVariant& p) noexcept;
		void ModifygForPhaseHG(const Support::ParamVariant& p) noexcept;

		std::unique_ptr<Render::PrivateData> m_data;

		struct Defaults
		{		
			// Ref: S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," Computer Graphics Forum, 2020.
			inline static constexpr Math::float3 SIGMA_S_RAYLEIGH = Math::float3(5.802f, 13.558f, 33.1f) * 1e-3f;	// 1 / km
			inline static constexpr float SIGMA_S_MIE = 3.996f * 1e-3f;		// Mie scattering is not wavelength-dependant
			inline static constexpr float SIGMA_A_MIE = 4.4f * 1e-3f;
			inline static constexpr Math::float3 SIGMA_A_OZONE = Math::float3(0.65f, 1.881f, 0.085f) * 1e-3f;
			static constexpr float g = 0.8f;
			static constexpr float ATMOSPHERE_ALTITUDE = 100.0f;		// km
			static constexpr float PLANET_RADIUS = 6360.0f;				// km
			static constexpr float RAY_T_OFFSET = 5e-4;
		};
	};
}

namespace ZetaRay::Scene::Settings
{
	enum class AA
	{
		NATIVE,
		NATIVE_TAA,
		POINT,
		FSR2,
		COUNT
	};

	enum class DENOISER
	{
		NONE,
		STAD,
		COUNT
	};
}