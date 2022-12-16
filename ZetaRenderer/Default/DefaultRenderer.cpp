#include "DefaultRenderer.h"
#include "DefaultRendererImpl.h"
#include <App/Timer.h>
#include <App/Log.h>
#include <Core/Direct3DHelpers.h>
#include <Core/SharedShaderResources.h>
#include <Support/Task.h>
#include <Support/Param.h>
#include <Math/MatrixFuncs.h>

namespace
{
	Data* g_data = nullptr;
}

using namespace ZetaRay;
using namespace ZetaRay::Scene;
using namespace ZetaRay::DefaultRenderer;
using namespace ZetaRay::DefaultRenderer::Settings;
using namespace ZetaRay::Math;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// DefaultRenderer::Common
//--------------------------------------------------------------------------------------

void Common::UpdateFrameConstants(cbFrameConstants& frameConsts, Core::DefaultHeapBuffer& frameConstsBuff,
	const GBufferData& gbuffData, const LightData& lightData) noexcept
{
	const int currIdx = App::GetTimer().GetTotalFrameCount() & 0x1;
	frameConsts.FrameNum = (uint32_t)App::GetTimer().GetTotalFrameCount();
	frameConsts.RenderWidth = App::GetRenderer().GetRenderWidth();
	frameConsts.RenderHeight = App::GetRenderer().GetRenderHeight();
	frameConsts.DisplayWidth = App::GetRenderer().GetDisplayWidth();
	frameConsts.DisplayHeight = App::GetRenderer().GetDisplayHeight();
	frameConsts.MipBias = App::GetUpscalingFactor() != 1.0f ?
		log2f((float)frameConsts.RenderWidth / frameConsts.DisplayWidth) - 1.0f :
		0.0f;

	frameConsts.BaseColorMapsDescHeapOffset = App::GetScene().GetBaseColMapsDescHeapOffset();
	frameConsts.NormalMapsDescHeapOffset = App::GetScene().GetNormalMapsDescHeapOffset();
	frameConsts.MetalnessRoughnessMapsDescHeapOffset = App::GetScene().GetMetalnessRougnessMapsDescHeapOffset();
	frameConsts.EmissiveMapsDescHeapOffset = App::GetScene().GetEmissiveMapsDescHeapOffset();

	// Note: assumes BVH has been built
	frameConsts.WorldRadius = App::GetScene().GetWorldAABB().Extents.length();

	// camera
	const Camera& cam = App::GetCamera();
	v_float4x4 vCurrV(const_cast<float4x4a&>(cam.GetCurrView()));
	v_float4x4 vP(const_cast<float4x4a&>(cam.GetCurrProj()));
	v_float4x4 vVP = mul(vCurrV, vP);

	frameConsts.CameraPos = cam.GetPos();
	frameConsts.CameraNear = cam.GetNearZ();
	frameConsts.AspectRatio = cam.GetAspectRatio();
	frameConsts.PixelSpreadAngle = cam.GetPixelSpreadAngle();
	frameConsts.TanHalfFOV = tanf(0.5f * cam.GetFOV());
	frameConsts.CurrProj = cam.GetCurrProj();
	frameConsts.PrevView = frameConsts.CurrView;
	frameConsts.CurrView = float3x4(cam.GetCurrView());
	frameConsts.PrevViewProj = frameConsts.CurrViewProj;
	frameConsts.CurrViewProj = store(vVP);
	frameConsts.PrevViewInv = frameConsts.CurrViewInv;
	frameConsts.CurrViewInv = float3x4(cam.GetViewInv());
	frameConsts.PrevCameraJitter = frameConsts.CurrCameraJitter;
	frameConsts.CurrCameraJitter = cam.GetProjOffset();

	// frame gbuffer srv desc. table
	frameConsts.CurrGBufferDescHeapOffset = gbuffData.SRVDescTable[currIdx].GPUDesciptorHeapIndex();
	frameConsts.PrevGBufferDescHeapOffset = gbuffData.SRVDescTable[1 - currIdx].GPUDesciptorHeapIndex();

	// env. map SRV
	frameConsts.EnvMapDescHeapOffset = lightData.GpuDescTable.GPUDesciptorHeapIndex(LightData::DESC_TABLE::ENV_MAP_SRV);

	frameConstsBuff = App::GetRenderer().GetGpuMemory().GetDefaultHeapBufferAndInit(GlobalResource::FRAME_CONSTANTS_BUFFER_NAME,
		Math::AlignUp(sizeof(cbFrameConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		false,
		&frameConsts);

	App::GetRenderer().GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(GlobalResource::FRAME_CONSTANTS_BUFFER_NAME,
		frameConstsBuff);
}

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer
{
	void SetIndierctDiffuseDenoiser(const ParamVariant& p) noexcept
	{
		const int e = p.GetEnum().m_curr;
		Assert(e < (int)DENOISER::COUNT, "invalid enum value");
		const DENOISER u = (DENOISER)e;

		if (u == DENOISER::NONE)
			g_data->m_settings.IndirectDiffuseDenoiser = DENOISER::NONE;
		else if (u == DENOISER::STAD)
			g_data->m_settings.IndirectDiffuseDenoiser = DENOISER::STAD;
	}

	void SetInscatteringEnablement(const ParamVariant& p) noexcept
	{
		g_data->m_settings.Inscattering = p.GetBool();
	}

	void SetAA(const ParamVariant& p) noexcept
	{
		const int e = p.GetEnum().m_curr;
		Assert(e < (int)AA::COUNT, "invalid enum value");
		const AA u = (AA)e;

		if (u == AA::NATIVE)
		{
			// following order is important
			g_data->m_settings.AntiAliasing = AA::NATIVE;

			if (g_data->m_postProcessorData.Fsr2Pass.IsInitialized())
				g_data->m_postProcessorData.Fsr2Pass.Reset();

			if (g_data->m_postProcessorData.TaaPass.IsInitialized())
				g_data->m_postProcessorData.TaaPass.Reset();

			App::SetUpscalingEnablement(false);
		}
		else if (u == AA::POINT)
		{
			// following order is important
			g_data->m_settings.AntiAliasing = AA::POINT;

			if (g_data->m_postProcessorData.Fsr2Pass.IsInitialized())
				g_data->m_postProcessorData.Fsr2Pass.Reset();

			if (g_data->m_postProcessorData.TaaPass.IsInitialized())
				g_data->m_postProcessorData.TaaPass.Reset();

			App::SetUpscalingEnablement(true);
		}
		else if (u == AA::FSR2)
		{
			// following order is important
			if (g_data->m_postProcessorData.TaaPass.IsInitialized())
				g_data->m_postProcessorData.TaaPass.Reset();

			App::SetUpscalingEnablement(true);
			g_data->m_settings.AntiAliasing = AA::FSR2;
		}
		else if (u == AA::NATIVE_TAA)
		{
			// following order is important
			if (g_data->m_postProcessorData.Fsr2Pass.IsInitialized())
				g_data->m_postProcessorData.Fsr2Pass.Reset();

			App::SetUpscalingEnablement(false);
			g_data->m_settings.AntiAliasing = AA::NATIVE_TAA;
		}
	}

	void RayOffset(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.RayOffset = p.GetFloat().m_val;
	}

	void ModifySunDir(const ParamVariant& p) noexcept
	{
		float pitch = p.GetUnitDir().m_pitch;
		float yaw = p.GetUnitDir().m_yaw;
		g_data->m_frameConstants.SunDir = -Math::SphericalToCartesian(pitch, yaw);
	}

	void ModifySunLux(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.SunIlluminance = p.GetFloat().m_val;
	}

	void ModifySunAngularRadius(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.SunCosAngularRadius = cosf(p.GetFloat().m_val);
	}

	void ModifyRayleighSigmaSColor(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.RayleighSigmaSColor = p.GetFloat3().m_val;
	}

	void ModifyRayleighSigmaSScale(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.RayleighSigmaSScale = p.GetFloat().m_val;
	}

	void ModifyMieSigmaS(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.MieSigmaS = p.GetFloat().m_val;
	}

	void ModifyMieSigmaA(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.MieSigmaA = p.GetFloat().m_val;
	}

	void ModifyOzoneSigmaAColor(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.OzoneSigmaAColor = p.GetColor().m_val;
	}

	void ModifyOzoneSigmaAScale(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.OzoneSigmaAScale = p.GetFloat().m_val;
	}

	void ModifygForPhaseHG(const ParamVariant& p) noexcept
	{
		g_data->m_frameConstants.g = p.GetFloat().m_val;
	}
}

namespace ZetaRay::DefaultRenderer
{
	void Init() noexcept
	{
		g_data->m_renderGraph.Reset();

		const Camera& cam = App::GetCamera();
		v_float4x4 vCurrV(const_cast<float4x4a&>(cam.GetCurrView()));
		v_float4x4 vP(const_cast<float4x4a&>(cam.GetCurrProj()));
		v_float4x4 vVP = mul(vCurrV, vP);

		// for 1st frame
		//data->m_frameConstants.UseReverseZ = RendererConstants::USE_REVERSE_Z;
		g_data->m_frameConstants.PrevViewProj = store(vVP);
		g_data->m_frameConstants.PrevViewInv = float3x4(cam.GetViewInv());
		g_data->m_frameConstants.PrevView = float3x4(cam.GetCurrView());
		g_data->m_frameConstants.RayOffset = 3e-2f;

		g_data->m_frameConstants.SunDir = float3(0.223f, -0.96f, -0.167f);
		//g_data->m_frameConstants.SunDir = float3(0.6169695854187012, -0.6370740532875061, -0.4620445668697357);
		g_data->m_frameConstants.SunDir.normalize();
		//g_data->m_frameConstants.SunIlluminance = 50.0f;
		g_data->m_frameConstants.SunIlluminance = 187.0f;
		// sun angular diamter ~ 0.545 degrees 
		// 0.5 degrees == 0.0087266 radians 
		// cos(0.0087266 / 2)
		g_data->m_frameConstants.SunCosAngularRadius = 0.99998869f;
		g_data->m_frameConstants.AtmosphereAltitude = Defaults::ATMOSPHERE_ALTITUDE;
		g_data->m_frameConstants.PlanetRadius = Defaults::PLANET_RADIUS;
		g_data->m_frameConstants.g = Defaults::g;

		auto normalizeAndStore = [](float3 v, float3& cbVal, float& cbScale)
		{
			float scale = v.length();
			cbScale = scale;

			if (scale >= FLT_EPSILON)
			{
				float scaleRcp = 1.0f / scale;
				cbVal = v * scaleRcp;
			}
		};

		normalizeAndStore(Defaults::SIGMA_S_RAYLEIGH, g_data->m_frameConstants.RayleighSigmaSColor, g_data->m_frameConstants.RayleighSigmaSScale);
		normalizeAndStore(float3(Defaults::SIGMA_A_OZONE), g_data->m_frameConstants.OzoneSigmaAColor, g_data->m_frameConstants.OzoneSigmaAScale);

		g_data->m_frameConstants.MieSigmaA = Defaults::SIGMA_A_MIE;
		g_data->m_frameConstants.MieSigmaS = Defaults::SIGMA_S_MIE;

		TaskSet ts;
		auto h0 = ts.EmplaceTask("GBuffer_Init", []()
			{
				GBuffer::Init(g_data->m_settings, g_data->m_gBuffData);
			});
		auto h1 = ts.EmplaceTask("Light_Init", []()
			{
				Light::Init(g_data->m_settings, g_data->m_lightData);
			});
		auto h2 = ts.EmplaceTask("RayTracer_Init", []()
			{
				RayTracer::Init(g_data->m_settings, g_data->m_raytracerData);
			});
		auto h3 = ts.EmplaceTask("PostProcessor_Init", []()
			{
				PostProcessor::Init(g_data->m_settings, g_data->m_postProcessorData, g_data->m_lightData);
			});

		ts.AddOutgoingEdge(h1, h3);
		ts.Sort();
		ts.Finalize();
		App::Submit(ZetaMove(ts));

		// render settings
		{
			//ParamVariant enableIndirectDiffuse;
			//enableIndirectDiffuse.InitBool("Renderer", "Settings", "RaytracedIndirectDiffuse",
			//	fastdelegate::MakeDelegate(this, &SceneRenderer::SetIndirectDiffuseEnablement),
			//	m_g_data->m_settings.RTIndirectDiffuse);
			//App::AddParam(enableIndirectDiffuse);

			//ParamVariant enableDenoiser;
			//enableDenoiser.InitEnum("Renderer", "Settings", "IndirectDiffuseDenoiser",
			//	fastdelegate::MakeDelegate(this, &SceneRenderer::SetIndierctDiffuseDenoiser),
			//	Denoisers, ZetaArrayLen(Denoisers), (int)DENOISER::STAD);
			//App::AddParam(enableDenoiser);

			ParamVariant enableInscattering;
			enableInscattering.InitBool("Renderer", "Settings", "Inscattering",
				fastdelegate::FastDelegate1(&DefaultRenderer::SetInscatteringEnablement),
				g_data->m_settings.Inscattering);
			App::AddParam(enableInscattering);

			ParamVariant p6;
			p6.InitEnum("Renderer", "Settings", "Upscaling/AA", 
				fastdelegate::FastDelegate1(&DefaultRenderer::SetAA),
				AAOptions, ZetaArrayLen(AAOptions), (int)AA::NATIVE);
			App::AddParam(p6);
		}

		ParamVariant rayOffset;
		rayOffset.InitFloat("Renderer", "General", "RayOffset", 
			fastdelegate::FastDelegate1(&DefaultRenderer::RayOffset),
			Defaults::RAY_T_OFFSET,
			1e-4f,
			1e-1f,
			1e-4f);
		App::AddParam(rayOffset);

		// sun
		{
			ParamVariant p0;
			p0.InitUnitDir("LightSource", "Sun", "(-)Dir",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifySunDir),
				g_data->m_frameConstants.SunDir);
			App::AddParam(p0);

			ParamVariant p2;
			p2.InitFloat("LightSource", "Sun", "Illuminance",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifySunLux),
				g_data->m_frameConstants.SunIlluminance,
				1.0f,
				1000.0f,
				1.0f);
			App::AddParam(p2);

			ParamVariant p3;
			p3.InitFloat("LightSource", "Sun", "AngularRadius",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifySunAngularRadius),
				acosf(g_data->m_frameConstants.SunCosAngularRadius),
				0.004f,
				0.02f,
				1e-3f);
			App::AddParam(p3);
		}

		// atmosphere
		{
			ParamVariant p0;
			p0.InitColor("Scene", "Atmosphere", "Rayleigh scattering color",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifyRayleighSigmaSColor),
				g_data->m_frameConstants.RayleighSigmaSColor);
			App::AddParam(p0);

			ParamVariant p1;
			p1.InitFloat("Scene", "Atmosphere", "Rayleigh scattering scale",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifyRayleighSigmaSScale),
				g_data->m_frameConstants.RayleighSigmaSScale,
				0.0f,
				10.0f,
				3.0f);
			App::AddParam(p1);

			ParamVariant p2;
			p2.InitFloat("Scene", "Atmosphere", "Mie scattering coeff.",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifyMieSigmaS),
				Defaults::SIGMA_S_MIE,
				1e-6f,
				1e-1f,
				1e-3f);
			App::AddParam(p2);

			ParamVariant p3;
			p3.InitFloat("Scene", "Atmosphere", "Mie absorption coeff.",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifyMieSigmaA),
				Defaults::SIGMA_A_MIE,
				1e-6f,
				10.0f,
				1e-3f);
			App::AddParam(p3);

			ParamVariant p4;
			p4.InitFloat("Scene", "Atmosphere", "Ozone absorption scale",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifyOzoneSigmaAScale),
				g_data->m_frameConstants.OzoneSigmaAScale,
				0.0f,
				10.0f,
				3.0f);
			App::AddParam(p4);

			ParamVariant p6;
			p6.InitColor("Scene", "Atmosphere", "Ozone absorption color",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifyOzoneSigmaAColor),
				g_data->m_frameConstants.OzoneSigmaAColor);
			App::AddParam(p6);

			ParamVariant p7;
			p7.InitFloat("Scene", "Atmosphere", "g (HG Phase Function)",
				fastdelegate::FastDelegate1(&DefaultRenderer::ModifygForPhaseHG),
				Defaults::g,
				-0.99f,
				0.99f,
				0.2f);
			App::AddParam(p7);
		}
	}

	void Update(TaskSet& ts) noexcept
	{
		auto h0 = ts.EmplaceTask("SceneRenderer::Update_GBuffLight", []()
			{
				GBuffer::Update(g_data->m_gBuffData, g_data->m_lightData);
				Common::UpdateFrameConstants(g_data->m_frameConstants, g_data->m_frameConstantsBuff, g_data->m_gBuffData, g_data->m_lightData);
			});

		auto h1 = ts.EmplaceTask("SceneRenderer::Update_RT", []()
			{
				RayTracer::Update(g_data->m_settings, g_data->m_raytracerData);
				Light::Update(g_data->m_settings, g_data->m_gBuffData, g_data->m_raytracerData, g_data->m_lightData);
			});

		auto h2 = ts.EmplaceTask("SceneRenderer::Update_RT_Graph", []()
			{
				PostProcessor::Update(g_data->m_settings, g_data->m_gBuffData, g_data->m_lightData,
					g_data->m_raytracerData, g_data->m_postProcessorData);

				g_data->m_renderGraph.BeginFrame();

				GBuffer::Register(g_data->m_gBuffData, g_data->m_renderGraph);
				Light::Register(g_data->m_settings, g_data->m_raytracerData, g_data->m_lightData, g_data->m_renderGraph);
				RayTracer::Register(g_data->m_settings, g_data->m_raytracerData, g_data->m_renderGraph);
				PostProcessor::Register(g_data->m_settings, g_data->m_postProcessorData, g_data->m_renderGraph);

				g_data->m_renderGraph.MoveToPostRegister();

				GBuffer::DeclareAdjacencies(g_data->m_gBuffData, g_data->m_lightData, g_data->m_renderGraph);
				Light::DeclareAdjacencies(g_data->m_settings, g_data->m_gBuffData, g_data->m_raytracerData,
					g_data->m_lightData, g_data->m_renderGraph);
				RayTracer::DeclareAdjacencies(g_data->m_settings, g_data->m_gBuffData, g_data->m_raytracerData, g_data->m_renderGraph);
				PostProcessor::DeclareAdjacencies(g_data->m_settings, g_data->m_gBuffData, g_data->m_lightData,
					g_data->m_raytracerData, g_data->m_postProcessorData, g_data->m_renderGraph);
			});

		// RenderGraph should update last
		ts.AddOutgoingEdge(h0, h2);
		ts.AddOutgoingEdge(h1, h2);
	}

	void Render(TaskSet& ts) noexcept
	{
		g_data->m_renderGraph.Build(ts);
	}

	void Shutdown() noexcept
	{
		GBuffer::Shutdown(g_data->m_gBuffData);
		Light::Shutdown(g_data->m_lightData);
		RayTracer::Shutdown(g_data->m_raytracerData);
		PostProcessor::Shutdown(g_data->m_postProcessorData);

		g_data->m_frameConstantsBuff.Reset();
		g_data->m_renderGraph.Shutdown();

		delete g_data;
	}

	void OnWindowSizeChanged() noexcept
	{
		// following order is important
		GBuffer::OnWindowSizeChanged(g_data->m_settings, g_data->m_gBuffData);
		Light::OnWindowSizeChanged(g_data->m_settings, g_data->m_lightData);
		RayTracer::OnWindowSizeChanged(g_data->m_settings, g_data->m_raytracerData);
		PostProcessor::OnWindowSizeChanged(g_data->m_settings, g_data->m_postProcessorData, g_data->m_lightData);

		g_data->m_renderGraph.Reset();
	}

	void DebugDrawRenderGraph() noexcept
	{
		g_data->m_renderGraph.DebugDrawGraph();
	}
}

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

Scene::Renderer::Interface DefaultRenderer::InitAndGetInterface() noexcept
{
	Assert(!g_data, "g_data has already been initialized.");
	g_data = new (std::nothrow) Data;

	Scene::Renderer::Interface rndIntrf;

	rndIntrf.Init = &DefaultRenderer::Init;
	rndIntrf.Update = &DefaultRenderer::Update;
	rndIntrf.Render = &DefaultRenderer::Render;
	rndIntrf.Shutdown = &DefaultRenderer::Shutdown;
	rndIntrf.OnWindowSizeChanged = &DefaultRenderer::OnWindowSizeChanged;
	rndIntrf.DebugDrawRenderGraph = &DefaultRenderer::DebugDrawRenderGraph;

	return rndIntrf;
}