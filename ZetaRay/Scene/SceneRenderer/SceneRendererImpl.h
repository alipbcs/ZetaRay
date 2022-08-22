#pragma once

#include "SceneRenderer.h"
#include "../Scene.h"
#include "../../Core/Renderer.h"
#include "../../Math/Common.h"
#include "../../Math/Sampling.h"
#include "../../RenderPass/Common/FrameConstants.h"
#include "../../RenderPass/IndirectDiffuse/IndirectDiffuse.h"
#include "../../RenderPass/Clear/ClearPass.h"
#include "../../RenderPass/GBuffer/GBufferPass.h"
#include "../../RenderPass/Sun/SunLight.h"
#include "../../RenderPass/Sky/SkyDome.h"
#include "../../RenderPass/Compositing/Compositing.h"
#include "../../RenderPass/TAA/TAA.h"
#include "../../RenderPass/LuminanceReduction/LuminanceReduction.h"
#include "../../RenderPass/Final/FinalPass.h"
#include "../../RenderPass/GUI/GuiPass.h"
#include "../../RenderPass/Sky/Sky.h"
#include "../../RenderPass/SVGF/LinearDepthGradient.h"
#include "../../RenderPass/SVGF/SVGF.h"
#include "../../RayTracing/RtAccelerationStructure.h"
#include "../../RayTracing/Sampler.h"
#include "../../RenderPass/FSR2/FSR2.h"

//--------------------------------------------------------------------------------------
// SceneRenderer::PrivateData
//--------------------------------------------------------------------------------------

using Data = ZetaRay::SceneRenderer::PrivateData;

namespace ZetaRay
{
	struct alignas(64) RenderSettings
	{
		bool SunLighting = true;
		bool Inscattering = false;
		bool RTIndirectDiffuse = true;
		bool DenoiseIndirectDiffuseLi = false;
		bool TAA = false;
		bool Fsr2 = false;
	};

	struct alignas(64) GBufferRendererData
	{
		enum GBUFFER
		{
			GBUFFER_BASE_COLOR,
			GBUFFER_NORMAL_CURV,
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
		Texture BaseColor[2];
		Texture Normal[2];
		Texture MetalnessRoughness[2];
		Texture MotionVec;
		Texture EmissiveColor;
		Texture DepthBuffer[2];

		DescriptorTable SRVDescTable[2];
		DescriptorTable RTVDescTable[2];
		DescriptorTable DSVDescTable[2];

		static constexpr int MAX_NUM_RENDER_PASSES = 3;
		int NumRenderPasses = 1;

		RenderPass::GBufferPass RenderPasses[MAX_NUM_RENDER_PASSES];
		RenderNodeHandle Handles[MAX_NUM_RENDER_PASSES];

		RenderPass::ClearPass ClearPass;
		RenderNodeHandle ClearHandle;
	};

	struct alignas(64) LightManagerData
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
		DescriptorTable GpuDescTable;

		// HDR Light-accumulation texture
		Texture HdrLightAccumTex;
		DescriptorTable HdrLightAccumRTV;

		// Environment Light
		//Texture EnvLightTex;
		//DescriptorTable EnvMapDescTable;
		//EnvLightDesc EnvLightDesc;
		//DefaultHeapBuffer EnvMapAliasTableBuffer;
		//DefaultHeapBuffer EnvMapPatchBuffer;

		// Emissive Triangle
		SmallVector<EmissiveTriangle> EmissiveTriangles;		// maintain a system memory copy
		DefaultHeapBuffer EmissiveTrianglesBuff;		// GPU buffer
//		SmallVector<EmissiveTriUpdateInstance> EmissiveUpdateBatch;
		DefaultHeapBuffer EmissiveAliasTable;

		// Render Passes
		RenderPass::SunLight SunLightPass;
		RenderNodeHandle SunLightPassHandle;

		RenderPass::SkyDome SkyDomePass;
		RenderNodeHandle SkyDomePassHandle;

		RenderPass::Compositing CompositPass;
		RenderNodeHandle CompositPassHandle;

		RenderPass::Sky SkyPass;
		RenderNodeHandle SkyPassHandle;
	};

	struct alignas(64) PostProcessData
	{
		// Render Passes
		RenderPass::TAA TaaPass;
		RenderNodeHandle TaaHandle;

		RenderPass::FSR2Pass Fsr2Pass;
		RenderNodeHandle Fsr2PassHandle;

		RenderPass::LuminanceReduction LumReductionPass;
		RenderNodeHandle LumReductionPassHandle;

		RenderPass::FinalPass FinalDrawPass;
		RenderNodeHandle FinalPassHandle;

		RenderPass::GuiPass ImGuiPass;
		RenderNodeHandle ImGuiPassHandle;

		// Descriptors
		DescriptorTable TaaOrFsr2OutSRV;
		DescriptorTable HdrLightAccumSRV;
		DescriptorTable HdrLightAccumRTV;
	};

	struct alignas(64) RayTracerData
	{
		// Scene BVH
		TLAS RtAS;

		// Sampler
		Sampler RtSampler;

		// Render Passes
		RenderNodeHandle RtASBuildPassHandle;

		RenderPass::IndirectDiffuse IndirectDiffusePass;
		RenderNodeHandle IndirectDiffusePassHandle;

		RenderPass::LinearDepthGradient LinearDepthGradPass;
		RenderNodeHandle LinearDepthGradPassHandle;

		RenderPass::SVGF SVGF_Pass;
		RenderNodeHandle SVGF_PassHandle;

		// Descriptors
		enum DESC_TABLE
		{
			LINEAR_DEPTH_GRAD,
			SPATIAL_VAR,
			TEMPORAL_CACHE,
			INDIRECT_LO,
			COUNT
		};

		DescriptorTable DescTableAll;
	};

	struct SceneRenderer::PrivateData
	{
		cbFrameConstants m_frameConstants;
		RenderSettings m_settings;

		GBufferRendererData m_gBuffData;
		LightManagerData m_lightManagerData;
		PostProcessData m_postProcessorData;
		RayTracerData m_raytracerData;
	};
}

//--------------------------------------------------------------------------------------
// GBufferRenderer
//--------------------------------------------------------------------------------------

namespace ZetaRay::GBufferRenderer
{
	void Init(const RenderSettings& settings, GBufferRendererData& data) noexcept;
	void CreateGBuffers(GBufferRendererData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, GBufferRendererData& data) noexcept;
	void Shutdown(GBufferRendererData& data) noexcept;

	// Assigns meshes to GBufferRenderPass instances and prepares draw call arguments
	void Update(GBufferRendererData& gbuffData, const LightManagerData& lightManagerData) noexcept;
	void Register(GBufferRendererData& data, RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(GBufferRendererData& data, const LightManagerData& lightManagerData, RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// LightManager
//--------------------------------------------------------------------------------------

namespace ZetaRay::LightManager
{
	void Init(const RenderSettings& settings, LightManagerData& data) noexcept;
	void CreateHDRLightAccumTex(LightManagerData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, LightManagerData& data) noexcept;
	void Shutdown(LightManagerData& data) noexcept;

	void SetEnvMap(LightManagerData& data, const Win32::Filesystem::Path& pathToEnvLight, const Win32::Filesystem::Path& pathToPatches) noexcept;
	void AddEmissiveTriangle(LightManagerData& data, uint64_t instanceID, Vector<float, 32>&& lumen) noexcept;
	void Register(const RenderSettings& settings, const RayTracerData& rayTracerData, LightManagerData& data, RenderGraph& renderGraph) noexcept;
	void Update(const RenderSettings& settings, const GBufferRendererData& gbuffData, const RayTracerData& rayTracerData, 
		LightManagerData& lightManagerData) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, const GBufferRendererData& gbuffData, const RayTracerData& rayTracerData,
		LightManagerData& lightManagerData, RenderGraph& renderGraph) noexcept;

	// Updates the GPU Buffer that contains all the emissive triangles
//	void UpdateAnalyticalLightBuffers(LightManagerData& data) noexcept;
	void UpdateEmissiveTriangleBuffers(LightManagerData& data) noexcept;
}

//--------------------------------------------------------------------------------------
// RayTracer
//--------------------------------------------------------------------------------------

namespace ZetaRay::RayTracer
{
	void Init(const RenderSettings& settings, RayTracerData& data) noexcept;
	void CreateDescriptors(const RenderSettings& settings, RayTracerData& data) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Shutdown(RayTracerData& data) noexcept;

	void Update(const RenderSettings& settings, RayTracerData& data) noexcept;
	void Register(const RenderSettings& settings, RayTracerData& data, RenderGraph& renderGraph) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, const GBufferRendererData& gbuffData, RayTracerData& rtData, RenderGraph& renderGraph) noexcept;
}

//--------------------------------------------------------------------------------------
// PostProcessor
//--------------------------------------------------------------------------------------

namespace ZetaRay::PostProcessor
{
	void Init(const RenderSettings& settings, PostProcessData& postData, const LightManagerData& lightManagerData) noexcept;
	void UpdateDescriptors(const RenderSettings& settings, const LightManagerData& lightManagerData, 
		PostProcessData& postData) noexcept;	
	void UpdatePasses(const RenderSettings& settings, PostProcessData& postData) noexcept;
	void OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& data, 
		const LightManagerData& lightManagerData) noexcept;
	void Shutdown(PostProcessData& data) noexcept;

	void Register(const RenderSettings& settings, PostProcessData& data, RenderGraph& renderGraph) noexcept;
	void Update(const RenderSettings& settings, const GBufferRendererData& gbuffData, const LightManagerData& lightManagerData, 
		const RayTracerData& rayTracerData, PostProcessData& data) noexcept;
	void DeclareAdjacencies(const RenderSettings& settings, const GBufferRendererData& gbuffData, const LightManagerData& lightManagerData,
		const RayTracerData& rayTracerData, PostProcessData& postData, RenderGraph& renderGraph) noexcept;
}