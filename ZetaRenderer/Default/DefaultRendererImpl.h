#pragma once

#include <Scene/SceneCore.h>
#include <Core/RendererCore.h>
#include <Core/RenderGraph.h>
//#include <Math/Sampling.h>
#include <Common/FrameConstants.h>
#include <IndirectDiffuse/ReSTIR_GI_Diffuse.h>
#include <Clear/Clear.h>
#include <GBuffer/GBufferPass.h>
#include <SunShadow/SunShadow.h>
#include <Sky/SkyDome.h>
#include <Compositing/Compositing.h>
#include <TAA/TAA.h>
#include <AutoExposure/AutoExposure.h>
#include <Display/Display.h>
#include <GUI/GuiPass.h>
#include <Sky/Sky.h>
#include <Denoiser/DiffuseDNSR.h>
#include <RayTracing/RtAccelerationStructure.h>
#include <RayTracing/Sampler.h>
#include <FSR2/FSR2.h>

// Note: with a functional-style API dependencies become more clear, which 
// results in fewer data-race issues and simpler debugging

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::Settings
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

namespace ZetaRay::DefaultRenderer
{
	inline static const char* Denoisers[] = { "None", "STAD" };
	static_assert((int)Settings::DENOISER::COUNT == ZetaArrayLen(Denoisers), "enum <-> strings mismatch.");
	inline static const char* AAOptions[] = { "Native", "Native+TAA", "Point", "AMD FSR 2.0 (Quality)" };
	static_assert((int)Settings::AA::COUNT == ZetaArrayLen(AAOptions), "enum <-> strings mismatch.");

	struct alignas(64) RenderSettings
	{
		bool Inscattering = false;
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

		enum class DESC_TABLE_CONST
		{
			HDR_LIGHT_ACCUM_UAV,
			ENV_MAP_SRV,
			INSCATTERING_SRV,
			COUNT
		};

		enum class DESC_TABLE_PER_FRAME
		{
			RAW_SHADOW_MASK,
			DENOISED_SHADOW_MASK,
			COUNT
		};

		Core::DescriptorTable GpuDescTable;
		Core::DescriptorTable SunShadowGpuDescTable;

		// HDR Light-accumulation texture
		Core::Texture HdrLightAccumTex;
		Core::DescriptorTable HdrLightAccumRTV;

		// Render Passes
		RenderPass::SunShadow SunShadowPass;
		Core::RenderNodeHandle SunShadowHandle;

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

		RenderPass::AutoExposure AutoExposurePass;
		Core::RenderNodeHandle AutoExposureHandle;

		RenderPass::DisplayPass DisplayPass;
		Core::RenderNodeHandle DisplayHandle;

		RenderPass::GuiPass GuiPass;
		Core::RenderNodeHandle GuiHandle;

		// Descriptors
		enum class DESC_TABLE_CONST
		{
			HDR_LIGHT_ACCUM_SRV,
			EXPOSURE_SRV,
			COUNT
		};

		Core::DescriptorTable WindowSizeConstSRVs;
		Core::DescriptorTable TaaOrFsr2OutSRV;
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

		RenderPass::ReSTIR_GI_Diffuse ReSTIR_GI_DiffusePass;
		Core::RenderNodeHandle ReSTIR_GIHandle;

		RenderPass::DiffuseDNSR DiffuseDNSRPass;
		Core::RenderNodeHandle DiffuseDNSRHandle;

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

		GBufferData m_gbuffData;
		LightData m_lightData;
		PostProcessData m_postProcessorData;
		RayTracerData m_raytracerData;
	};

	struct Defaults
	{
		// Ref: S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," Computer Graphics Forum, 2020.
		inline static constexpr Math::float3 SIGMA_S_RAYLEIGH = Math::float3(5.802f, 13.558f, 33.1f) * 1e-3f;	// 1 / km
		inline static constexpr float SIGMA_S_MIE = 3.996f * 1e-3f;		// Mie scattering is not wavelength dependent
		inline static constexpr float SIGMA_A_MIE = 4.4f * 1e-3f;
		inline static constexpr Math::float3 SIGMA_A_OZONE = Math::float3(0.65f, 1.881f, 0.085f) * 1e-3f;
		static constexpr float g = 0.8f;
		static constexpr float ATMOSPHERE_ALTITUDE = 100.0f;		// km
		static constexpr float PLANET_RADIUS = 6360.0f;				// km
		static constexpr float RAY_T_OFFSET = 5e-4;
	};
}

using Data = ZetaRay::DefaultRenderer::PrivateData;

//--------------------------------------------------------------------------------------
// Common
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::Common
{
	void UpdateFrameConstants(cbFrameConstants& frameConsts, Core::DefaultHeapBuffer& frameConstsBuff, 
		const GBufferData& gbuffData, const LightData& lightData) noexcept;
}

//--------------------------------------------------------------------------------------
// GBuffer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::GBuffer
{
	void Init(const RenderSettings& settings, GBufferData& data) noexcept;
	void CreateGBuffers(GBufferData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, GBufferData& data) noexcept;
	void Shutdown(GBufferData& data) noexcept;

	// Assigns meshes to GBufferRenderPass instances and prepares draw call arguments
	void Update(GBufferData& gbuffData) noexcept;
	void Register(GBufferData& data, Core::RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(GBufferData& data, const LightData& lightManagerData, Core::RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// Light
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::Light
{
	void Init(const RenderSettings& settings, LightData& data) noexcept;
	void CreateHDRLightAccumTex(LightData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, LightData& data) noexcept;
	void Shutdown(LightData& data) noexcept;

	void Register(const RenderSettings& settings, LightData& data, const RayTracerData& rayTracerData, 
		Core::RenderGraph& renderGraph) noexcept;
	void Update(const RenderSettings& settings, LightData& lightData, const GBufferData& gbuffData, 
		const RayTracerData& rayTracerData) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, LightData& lightData, const GBufferData& gbuffData, 
		const RayTracerData& rayTracerData, Core::RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::RayTracer
{
	void Init(const RenderSettings& settings, RayTracerData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Shutdown(RayTracerData& data) noexcept;

	void UpdateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept;
	void UpdatePasses(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Update(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Register(const RenderSettings& settings, RayTracerData& data, Core::RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, RayTracerData& rtData, const GBufferData& gbuffData, 
		Core::RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// PostProcessor
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer::PostProcessor
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