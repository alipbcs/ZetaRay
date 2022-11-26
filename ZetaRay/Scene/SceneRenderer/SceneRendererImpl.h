#pragma once

#include "SceneRenderer.h"
#include "../SceneCore.h"
#include "../../Core/Renderer.h"
#include "../../Core/RenderGraph.h"
#include "../../Math/Sampling.h"
#include "../../RenderPass/Common/FrameConstants.h"
#include "../../RenderPass/IndirectDiffuse/ReSTIR_GI.h"
#include "../../RenderPass/Clear/Clear.h"
#include "../../RenderPass/GBuffer/GBuffer.h"
#include "../../RenderPass/Sun/Sun.h"
#include "../../RenderPass/Sky/SkyDome.h"
#include "../../RenderPass/Compositing/Compositing.h"
#include "../../RenderPass/TAA/TAA.h"
#include "../../RenderPass/LuminanceReduction/LuminanceReduction.h"
#include "../../RenderPass/Final/FinalPass.h"
#include "../../RenderPass/GUI/GuiPass.h"
#include "../../RenderPass/Sky/Sky.h"
#include "../../RenderPass/Denoiser/STAD.h"
#include "../../RayTracing/RtAccelerationStructure.h"
#include "../../RayTracing/Sampler.h"
#include "../../RenderPass/FSR2/FSR2.h"

// Note: with a functional-style API dependencies become more clear, which 
// results in fewer data-race issues and simpler debugging

//--------------------------------------------------------------------------------------
// SceneRenderer::PrivateData
//--------------------------------------------------------------------------------------

using Data = ZetaRay::Scene::Render::PrivateData;

namespace ZetaRay::Scene::Render
{
	inline static const char* Denoisers[] = { "None", "STAD" };
	static_assert((int)Settings::DENOISER::COUNT == ZetaArrayLen(Denoisers), "enum <-> strings mismatch.");
	inline static const char* AAOptions[] = { "Native", "Native+TAA", "Point", "AMD FSR 2.0 (Quality)" };
	static_assert((int)Settings::AA::COUNT == ZetaArrayLen(AAOptions), "enum <-> strings mismatch.");

	struct alignas(64) RenderSettings
	{
		bool SunLighting = true;
		bool Inscattering = false;
		bool RTIndirectDiffuse = true;
		Settings::DENOISER IndirectDiffuseDenoiser = Settings::DENOISER::STAD;
		Settings::AA AntiAliasing = Settings::AA::NATIVE;
	};

	struct alignas(64) GBufferData
	{
		enum GBUFFER
		{
			GBUFFER_BASE_COLOR,
			GBUFFER_NORMAL,
			GBUFFER_METALNESS_ROUGHNESS,
			GBUFFER_MOTION_VECTOR,
			GBUFFER_EMISSIVE_COLOR,
			GBUFFER_DEPTH,
			COUNT
		};

		inline static const DXGI_FORMAT GBUFFER_FORMAT[GBUFFER::COUNT] =
		{
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			DXGI_FORMAT_R16G16_FLOAT,
			DXGI_FORMAT_R8G8_UNORM,
			DXGI_FORMAT_R16G16_FLOAT,
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			DXGI_FORMAT_D32_FLOAT
		};

		// previous frame's gbuffers are required for denoising and ReSTIR
		Core::Texture BaseColor[2];
		Core::Texture Normal[2];
		Core::Texture MetalnessRoughness[2];
		Core::Texture MotionVec;
		Core::Texture EmissiveColor;
		Core::Texture DepthBuffer[2];

		Core::DescriptorTable SRVDescTable[2];
		Core::DescriptorTable RTVDescTable[2];
		Core::DescriptorTable DSVDescTable[2];

		RenderPass::GBufferPass GBuffPass;
		Core::RenderNodeHandle GBuffPassHandle;

		RenderPass::ClearPass ClearPass;
		Core::RenderNodeHandle ClearHandle;
	};

	struct alignas(64) LightData
	{
		static const DXGI_FORMAT HDR_LIGHT_ACCUM_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
		static const int MAX_NUM_ENV_LIGHT_PATCHES = 128;
		static const int SKY_LUT_WIDTH = 256;
		static const int SKY_LUT_HEIGHT = 128;

//		struct EmissiveTriUpdateInstance
//		{
//			uint64_t InstanceID;
//			SmallVector<float, GetExcessSize(sizeof(float), alignof(float)), 32> Lumen;
//		};

		enum DESC_TABLE
		{
			HDR_LIGHT_ACCUM_UAV,
			ENV_MAP_SRV,
			INSCATTERING_SRV,
			COUNT
		};
		Core::DescriptorTable GpuDescTable;

		// HDR Light-accumulation texture
		Core::Texture HdrLightAccumTex;
		Core::DescriptorTable HdrLightAccumRTV;

		// Environment Light
		//Texture EnvLightTex;
		//DescriptorTable EnvMapDescTable;
		//EnvLightDesc EnvLightDesc;
		//DefaultHeapBuffer EnvMapAliasTableBuffer;
		//DefaultHeapBuffer EnvMapPatchBuffer;

		// Emissive Triangle
		Util::SmallVector<EmissiveTriangle> EmissiveTriangles;		// maintain a system memory copy
		Core::DefaultHeapBuffer EmissiveTrianglesBuff;		// GPU buffer
//		SmallVector<EmissiveTriUpdateInstance> EmissiveUpdateBatch;
		Core::DefaultHeapBuffer EmissiveAliasTable;

		// Render Passes
		RenderPass::SunLight SunLightPass;
		Core::RenderNodeHandle SunLightHandle;

		RenderPass::SkyDome SkyDomePass;
		Core::RenderNodeHandle SkyDomeHandle;

		RenderPass::Compositing CompositingPass;
		Core::RenderNodeHandle CompositingHandle;

		RenderPass::Sky SkyPass;
		Core::RenderNodeHandle SkyHandle;
	};

	struct alignas(64) PostProcessData
	{
		// Render Passes
		RenderPass::TAA TaaPass;
		Core::RenderNodeHandle TaaHandle;
		RenderPass::FSR2Pass Fsr2Pass;
		Core::RenderNodeHandle Fsr2Handle;

		RenderPass::LuminanceReduction LumReductionPass;
		Core::RenderNodeHandle LumReductionHandle;

		RenderPass::FinalPass FinalDrawPass;
		Core::RenderNodeHandle FinalHandle;

		RenderPass::GuiPass GuiPass;
		Core::RenderNodeHandle GuiHandle;

		// Descriptors
		Core::DescriptorTable TaaOrFsr2OutSRV;
		Core::DescriptorTable HdrLightAccumSRV;
		Core::DescriptorTable HdrLightAccumRTV;
	};

	struct alignas(64) RayTracerData
	{
		// Scene BVH
		RT::TLAS RtAS;

		// Sampler
		RT::Sampler RtSampler;

		// Render Passes
		Core::RenderNodeHandle RtASBuildHandle;

		RenderPass::ReSTIR_GI ReSTIR_GIPass;
		Core::RenderNodeHandle ReSTIR_GIHandle;

		RenderPass::STAD StadPass;
		Core::RenderNodeHandle StadHandle;

		// Descriptors
		enum DESC_TABLE
		{
			STAD_TEMPORAL_CACHE,
			TEMPORAL_RESERVOIR_A,
			TEMPORAL_RESERVOIR_B,
			TEMPORAL_RESERVOIR_C,
			SPATIAL_RESERVOIR_A,
			SPATIAL_RESERVOIR_B,
			SPATIAL_RESERVOIR_C,
			COUNT
		};

		Core::DescriptorTable DescTableAll;
	};

	struct PrivateData
	{
		Core::RenderGraph m_renderGraph;
		Core::DefaultHeapBuffer m_frameConstantsBuff;

		cbFrameConstants m_frameConstants;
		RenderSettings m_settings;

		GBufferData m_gBuffData;
		LightData m_lightData;
		PostProcessData m_postProcessorData;
		RayTracerData m_raytracerData;
	};
}

//--------------------------------------------------------------------------------------
// Common
//--------------------------------------------------------------------------------------

namespace ZetaRay::Scene::Render::Common
{
	void UpdateFrameConstants(cbFrameConstants& frameConsts, Core::DefaultHeapBuffer& frameConstsBuff, 
		const GBufferData& gbuffData, const LightData& lightData) noexcept;
}

//--------------------------------------------------------------------------------------
// GBuffer
//--------------------------------------------------------------------------------------

namespace ZetaRay::Scene::Render::GBuffer
{
	void Init(const RenderSettings& settings, GBufferData& data) noexcept;
	void CreateGBuffers(GBufferData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data) noexcept;
	void Shutdown(GBufferData& data) noexcept;

	// Assigns meshes to GBufferRenderPass instances and prepares draw call arguments
	void Update(GBufferData& gbuffData, const LightData& lightManagerData) noexcept;
	void Register(GBufferData& data, Core::RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(GBufferData& data, const LightData& lightManagerData, Core::RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// Light
//--------------------------------------------------------------------------------------

namespace ZetaRay::Scene::Render::Light
{
	void Init(const RenderSettings& settings, LightData& data) noexcept;
	void CreateHDRLightAccumTex(LightData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, LightData& data) noexcept;
	void Shutdown(LightData& data) noexcept;

	//void SetEnvMap(LightManagerData& data, const Win32::Filesystem::Path& pathToEnvLight, const Win32::Filesystem::Path& pathToPatches) noexcept;
	//void AddEmissiveTriangle(LightManagerData& data, uint64_t instanceID, Util::Vector<float, 32>&& lumen) noexcept;
	void Register(const RenderSettings& settings, const RayTracerData& rayTracerData, LightData& data, Core::RenderGraph& renderGraph) noexcept;
	void Update(const RenderSettings& settings, const GBufferData& gbuffData, const RayTracerData& rayTracerData,
		LightData& lightManagerData) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, const GBufferData& gbuffData, const RayTracerData& rayTracerData,
		LightData& lightManagerData, Core::RenderGraph& renderGraph) noexcept;

	// Updates the GPU Buffer that contains all the emissive triangles
	//void UpdateAnalyticalLightBuffers(LightManagerData& data) noexcept;
	//void UpdateEmissiveTriangleBuffers(LightManagerData& data) noexcept;
}

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

namespace ZetaRay::Scene::Render::RayTracer
{
	void Init(const RenderSettings& settings, RayTracerData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Shutdown(RayTracerData& data) noexcept;

	void UpdateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept;
	void UpdatePasses(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Update(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Register(const RenderSettings& settings, RayTracerData& data, Core::RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, const GBufferData& gbuffData, RayTracerData& rtData,
		Core::RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// PostProcessor
//--------------------------------------------------------------------------------------

namespace ZetaRay::Scene::Render::PostProcessor
{
	void Init(const RenderSettings& settings, PostProcessData& postData, const LightData& lightData) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data, 
		const LightData& lightData) noexcept;
	void Shutdown(PostProcessData& data) noexcept;

	void UpdateDescriptors(const RenderSettings& settings, PostProcessData& postData) noexcept;	
	void UpdatePasses(const RenderSettings& settings, PostProcessData& postData) noexcept;
	void Update(const RenderSettings& settings, const GBufferData& gbuffData, const LightData& lightData,
		const RayTracerData& rayTracerData, PostProcessData& data) noexcept;
	void Register(const RenderSettings& settings, PostProcessData& data, Core::RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, const GBufferData& gbuffData, const LightData& lightData,
		const RayTracerData& rayTracerData, PostProcessData& postData, Core::RenderGraph& renderGraph) noexcept;
}