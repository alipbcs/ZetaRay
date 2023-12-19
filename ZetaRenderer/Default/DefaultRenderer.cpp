#include "DefaultRenderer.h"
#include "DefaultRendererImpl.h"
#include <App/Timer.h>
#include <App/Log.h>
#include <Core/SharedShaderResources.h>
#include <Support/Task.h>
#include <Support/Param.h>
#include <Math/MatrixFuncs.h>
#include <Scene/Camera.h>

namespace
{
	Data* g_data = nullptr;
}

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
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

void Common::UpdateFrameConstants(cbFrameConstants& frameConsts, DefaultHeapBuffer& frameConstsBuff,
	const GBufferData& gbuffData, const LightData& lightData)
{
	auto& renderer = App::GetRenderer();
	const int currIdx = renderer.GlobaIdxForDoubleBufferedResources();

	frameConsts.FrameNum = (uint32_t)App::GetTimer().GetTotalFrameCount();
	frameConsts.dt = (float)App::GetTimer().GetElapsedTime();
	frameConsts.RenderWidth = renderer.GetRenderWidth();
	frameConsts.RenderHeight = renderer.GetRenderHeight();
	frameConsts.DisplayWidth = renderer.GetDisplayWidth();
	frameConsts.DisplayHeight = renderer.GetDisplayHeight();
	frameConsts.MipBias = App::GetUpscalingFactor() != 1.0f ?
		log2f((float)frameConsts.RenderWidth / frameConsts.DisplayWidth) - 1.0f :
		0.0f;

	frameConsts.BaseColorMapsDescHeapOffset = App::GetScene().GetBaseColMapsDescHeapOffset();
	frameConsts.NormalMapsDescHeapOffset = App::GetScene().GetNormalMapsDescHeapOffset();
	frameConsts.MetallicRoughnessMapsDescHeapOffset = App::GetScene().GetMetallicRougnessMapsDescHeapOffset();
	frameConsts.EmissiveMapsDescHeapOffset = App::GetScene().GetEmissiveMapsDescHeapOffset();

	// Note: assumes BVH has been built
	//frameConsts.WorldRadius = App::GetScene().GetWorldAABB().Extents.length();

	// camera
	const Camera& cam = App::GetCamera();
	v_float4x4 vCurrV = load4x4(const_cast<float4x4a&>(cam.GetCurrView()));
	v_float4x4 vP = load4x4(const_cast<float4x4a&>(cam.GetCurrProj()));
	v_float4x4 vVP = mul(vCurrV, vP);
	float3 prevCameraPos = frameConsts.CameraPos;

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
	frameConsts.PrevProjectionJitter = frameConsts.CurrProjectionJitter;
	frameConsts.CurrProjectionJitter = cam.GetProjOffset();

	// frame gbuffer srv desc. table
	frameConsts.CurrGBufferDescHeapOffset = gbuffData.SRVDescTable[currIdx].GPUDesciptorHeapIndex();
	frameConsts.PrevGBufferDescHeapOffset = gbuffData.SRVDescTable[1 - currIdx].GPUDesciptorHeapIndex();

	// env. map SRV
	frameConsts.EnvMapDescHeapOffset = lightData.ConstDescTable.GPUDesciptorHeapIndex((int)LightData::DESC_TABLE_CONST::ENV_MAP_SRV);

	float3 prevViewDir = float3(frameConsts.PrevViewInv.m[0].z, frameConsts.PrevViewInv.m[1].z, frameConsts.PrevViewInv.m[2].z);
	float3 currViewDir = float3(frameConsts.CurrViewInv.m[0].z, frameConsts.CurrViewInv.m[1].z, frameConsts.CurrViewInv.m[2].z);
	float3 one(1.0f);
	const bool cameraStatic = (one.dot(prevCameraPos - frameConsts.CameraPos) == 0) && (one.dot(prevViewDir - currViewDir) == 0);
	frameConsts.NumFramesCameraStatic = cameraStatic && frameConsts.Accumulate ? frameConsts.NumFramesCameraStatic + 1 : 1;
	frameConsts.CameraStatic = cameraStatic;

	constexpr uint32_t sizeInBytes = Math::AlignUp((uint32_t)sizeof(cbFrameConstants), (uint32_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	if (!frameConstsBuff.IsInitialized())
	{
		frameConstsBuff = GpuMemory::GetDefaultHeapBufferAndInit(GlobalResource::FRAME_CONSTANTS_BUFFER,
			sizeInBytes,
			false,
			&frameConsts);

		renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(GlobalResource::FRAME_CONSTANTS_BUFFER,
			frameConstsBuff);
	}
	else
		GpuMemory::UploadToDefaultHeapBuffer(frameConstsBuff, sizeInBytes, &frameConsts);
}

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

namespace ZetaRay::DefaultRenderer
{
	void SetInscatteringEnablement(const ParamVariant& p)
	{
		g_data->m_settings.Inscattering = p.GetBool();
	}

	void SetAA(const ParamVariant& p)
	{
		const int e = p.GetEnum().m_curr;
		Assert(e < (int)AA::COUNT, "invalid enum value");
		const AA u = (AA)e;

		if (u == g_data->m_settings.AntiAliasing)
			return;

		float newUpscaleFactor;
		g_data->PendingAA = u;

		switch (u)
		{
		case AA::FSR2:
			newUpscaleFactor = 1.5f;
			break;
		case AA::NONE:
		case AA::TAA:
		default:
			newUpscaleFactor = 1.0f;
			break;
		}

		App::SetUpscaleFactor(newUpscaleFactor);
	}

	void ModifySunDir(const ParamVariant& p)
	{
		float pitch = p.GetUnitDir().m_pitch;
		float yaw = p.GetUnitDir().m_yaw;
		g_data->m_frameConstants.SunDir = -Math::SphericalToCartesian(pitch, yaw);
	}

	void ModifySunLux(const ParamVariant& p)
	{
		g_data->m_frameConstants.SunIlluminance = p.GetFloat().m_val;
	}

	void ModifySunAngularRadius(const ParamVariant& p)
	{
		g_data->m_frameConstants.SunCosAngularRadius = cosf(p.GetFloat().m_val);
	}

	void ModifyRayleighSigmaSColor(const ParamVariant& p)
	{
		g_data->m_frameConstants.RayleighSigmaSColor = p.GetFloat3().m_val;
	}

	void ModifyRayleighSigmaSScale(const ParamVariant& p)
	{
		g_data->m_frameConstants.RayleighSigmaSScale = p.GetFloat().m_val;
	}

	void ModifyMieSigmaS(const ParamVariant& p)
	{
		g_data->m_frameConstants.MieSigmaS = p.GetFloat().m_val;
	}

	void ModifyMieSigmaA(const ParamVariant& p)
	{
		g_data->m_frameConstants.MieSigmaA = p.GetFloat().m_val;
	}

	void ModifyOzoneSigmaAColor(const ParamVariant& p)
	{
		g_data->m_frameConstants.OzoneSigmaAColor = p.GetColor().m_val;
	}

	void ModifyOzoneSigmaAScale(const ParamVariant& p)
	{
		g_data->m_frameConstants.OzoneSigmaAScale = p.GetFloat().m_val;
	}

	void ModifygForPhaseHG(const ParamVariant& p)
	{
		g_data->m_frameConstants.g = p.GetFloat().m_val;
	}

	void SetSkyIllumEnablement(const ParamVariant& p)
	{
		g_data->m_settings.SkyIllumination = p.GetBool();
		g_data->m_lightData.CompositingPass.SetSkyIllumEnablement(g_data->m_settings.SkyIllumination);
	}

	void SetAccumulation(const ParamVariant& p)
	{
		g_data->m_frameConstants.Accumulate = p.GetBool();
	}
}

namespace ZetaRay::DefaultRenderer
{
	void Init()
	{
		Assert(g_data->PendingAA == g_data->m_settings.AntiAliasing, "these must match.");

		g_data->m_renderGraph.Reset();

		const Camera& cam = App::GetCamera();
		v_float4x4 vCurrV = load4x4(const_cast<float4x4a&>(cam.GetCurrView()));
		v_float4x4 vP = load4x4(const_cast<float4x4a&>(cam.GetCurrProj()));
		v_float4x4 vVP = mul(vCurrV, vP);

		// for 1st frame
		g_data->m_frameConstants.PrevViewProj = store(vVP);
		g_data->m_frameConstants.PrevViewInv = float3x4(cam.GetViewInv());
		g_data->m_frameConstants.PrevView = float3x4(cam.GetCurrView());

		//g_data->m_frameConstants.SunDir = float3(0.223f, -0.96f, -0.167f);
		g_data->m_frameConstants.SunDir = float3(0.6565358f, -0.0560669f, 0.752208233f);
		//g_data->m_frameConstants.SunDir = float3(0.0f, 1.0f, 0.0f);
		g_data->m_frameConstants.SunDir.normalize();
		//g_data->m_frameConstants.SunIlluminance = 50.0f;
		g_data->m_frameConstants.SunIlluminance = 20.0f;
		// sun angular diamter ~ 0.545 degrees 
		// 0.5 degrees == 0.0087266 radians 
		// cos(0.0087266 / 2)
		g_data->m_frameConstants.SunCosAngularRadius = 0.99998869f;
		g_data->m_frameConstants.SunSinAngularRadius = sqrtf(1.0f - g_data->m_frameConstants.SunCosAngularRadius * g_data->m_frameConstants.SunCosAngularRadius);
		g_data->m_frameConstants.AtmosphereAltitude = Defaults::ATMOSPHERE_ALTITUDE;
		g_data->m_frameConstants.PlanetRadius = Defaults::PLANET_RADIUS;
		g_data->m_frameConstants.g = Defaults::g;
		g_data->m_frameConstants.NumFramesCameraStatic = 0;
		g_data->m_frameConstants.Accumulate = false;

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
				GBuffer::Init(g_data->m_settings, g_data->m_gbuffData);
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
			ParamVariant enableInscattering;
			enableInscattering.InitBool("Renderer", "Lighting", "Inscattering",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetInscatteringEnablement),
				g_data->m_settings.Inscattering);
			App::AddParam(enableInscattering);

			ParamVariant p6;
			p6.InitEnum("Renderer", "Anti-Aliasing", "AA/Upscaling",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetAA),
				AAOptions, ZetaArrayLen(AAOptions), (int)g_data->m_settings.AntiAliasing);
			App::AddParam(p6);
		}

		// sun
		{
			ParamVariant p0;
			p0.InitUnitDir("LightSource", "Sun", "(-)Dir",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifySunDir),
				-g_data->m_frameConstants.SunDir);
			App::AddParam(p0);

			ParamVariant p2;
			p2.InitFloat("LightSource", "Sun", "Illuminance",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifySunLux),
				g_data->m_frameConstants.SunIlluminance,
				1.0f,
				100.0f,
				1.0f);
			App::AddParam(p2);

			ParamVariant p3;
			p3.InitFloat("LightSource", "Sun", "Angular Radius",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifySunAngularRadius),
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
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifyRayleighSigmaSColor),
				g_data->m_frameConstants.RayleighSigmaSColor);
			App::AddParam(p0);

			ParamVariant p1;
			p1.InitFloat("Scene", "Atmosphere", "Rayleigh scattering scale",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifyRayleighSigmaSScale),
				g_data->m_frameConstants.RayleighSigmaSScale,
				0.0f,
				10.0f,
				1e-3f);
			App::AddParam(p1);

			ParamVariant p2;
			p2.InitFloat("Scene", "Atmosphere", "Mie scattering coeff.",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifyMieSigmaS),
				Defaults::SIGMA_S_MIE,
				1e-6f,
				1e-1f,
				1e-3f);
			App::AddParam(p2);

			ParamVariant p3;
			p3.InitFloat("Scene", "Atmosphere", "Mie absorption coeff.",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifyMieSigmaA),
				Defaults::SIGMA_A_MIE,
				1e-6f,
				10.0f,
				1e-3f);
			App::AddParam(p3);

			ParamVariant p4;
			p4.InitFloat("Scene", "Atmosphere", "Ozone absorption scale",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifyOzoneSigmaAScale),
				g_data->m_frameConstants.OzoneSigmaAScale,
				0.0f,
				10.0f,
				1e-4f);
			App::AddParam(p4);

			ParamVariant p6;
			p6.InitColor("Scene", "Atmosphere", "Ozone absorption color",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifyOzoneSigmaAColor),
				g_data->m_frameConstants.OzoneSigmaAColor);
			App::AddParam(p6);

			ParamVariant p7;
			p7.InitFloat("Scene", "Atmosphere", "g (HG Phase Function)",
				fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::ModifygForPhaseHG),
				Defaults::g,
				-0.99f,
				0.99f,
				0.2f);
			App::AddParam(p7);
		}

		ParamVariant p8;
		p8.InitBool("Renderer", "Lighting", "Sky Lighting", fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetSkyIllumEnablement),
			g_data->m_settings.SkyIllumination);
		App::AddParam(p8);

		ParamVariant p9;
		p9.InitBool("Renderer", "Lighting", "Accumulate", fastdelegate::FastDelegate1<const ParamVariant&>(&DefaultRenderer::SetAccumulation),
			g_data->m_frameConstants.Accumulate);
		App::AddParam(p9);
	}

	void Update(TaskSet& ts)
	{
		g_data->m_settings.AntiAliasing = g_data->PendingAA;

		auto h0 = ts.EmplaceTask("SceneRenderer::GBuff", []()
			{
				GBuffer::Update(g_data->m_gbuffData);
			});

		auto h1 = ts.EmplaceTask("SceneRenderer::RT_Post", []()
			{
				RayTracer::Update(g_data->m_settings, g_data->m_renderGraph, g_data->m_raytracerData);
				PostProcessor::Update(g_data->m_settings, g_data->m_postProcessorData, g_data->m_gbuffData,
					g_data->m_lightData, g_data->m_raytracerData);
			});

		auto h2 = ts.EmplaceTask("SceneRenderer::Light_FrameConsts", []()
			{
				Common::UpdateFrameConstants(g_data->m_frameConstants, g_data->m_frameConstantsBuff, g_data->m_gbuffData, g_data->m_lightData);
				Light::Update(g_data->m_settings, g_data->m_lightData, g_data->m_gbuffData, g_data->m_raytracerData);
			});

		auto h3 = ts.EmplaceTask("SceneRenderer::RenderGraph", []()
			{
				g_data->m_renderGraph.BeginFrame();

				GBuffer::Register(g_data->m_gbuffData, g_data->m_renderGraph);
				Light::Register(g_data->m_settings, g_data->m_lightData, g_data->m_raytracerData, g_data->m_renderGraph);
				RayTracer::Register(g_data->m_settings, g_data->m_raytracerData, g_data->m_renderGraph);
				PostProcessor::Register(g_data->m_settings, g_data->m_postProcessorData, g_data->m_renderGraph);

				g_data->m_renderGraph.MoveToPostRegister();

				GBuffer::DeclareAdjacencies(g_data->m_gbuffData, g_data->m_lightData, g_data->m_renderGraph);
				Light::DeclareAdjacencies(g_data->m_settings, g_data->m_lightData, g_data->m_gbuffData,
					g_data->m_raytracerData, g_data->m_renderGraph);
				RayTracer::DeclareAdjacencies(g_data->m_settings, g_data->m_raytracerData, g_data->m_gbuffData,
					g_data->m_renderGraph);
				PostProcessor::DeclareAdjacencies(g_data->m_settings, g_data->m_postProcessorData, g_data->m_gbuffData,
					g_data->m_lightData, g_data->m_raytracerData, g_data->m_renderGraph);
			});

		// RenderGraph should go last
		ts.AddOutgoingEdge(h0, h2);
		ts.AddOutgoingEdge(h2, h3);
		ts.AddOutgoingEdge(h1, h3);
	}

	void Render(TaskSet& ts)
	{
		g_data->m_renderGraph.Build(ts);
	}

	void Shutdown()
	{
		GBuffer::Shutdown(g_data->m_gbuffData);
		Light::Shutdown(g_data->m_lightData);
		RayTracer::Shutdown(g_data->m_raytracerData);
		PostProcessor::Shutdown(g_data->m_postProcessorData);

		g_data->m_frameConstantsBuff.Reset();
		g_data->m_renderGraph.Shutdown();

		delete g_data;
	}

	void OnWindowSizeChanged()
	{
		// following order is important
		GBuffer::OnWindowSizeChanged(g_data->m_settings, g_data->m_gbuffData);
		Light::OnWindowSizeChanged(g_data->m_settings, g_data->m_lightData);
		RayTracer::OnWindowSizeChanged(g_data->m_settings, g_data->m_raytracerData);
		PostProcessor::OnWindowSizeChanged(g_data->m_settings, g_data->m_postProcessorData, g_data->m_lightData);

		g_data->m_renderGraph.Reset();
	}

	Core::RenderGraph* GetRenderGraph()
	{
		return &g_data->m_renderGraph;
	}

	void DebugDrawRenderGraph()
	{
		g_data->m_renderGraph.DebugDrawGraph();
	}

	bool IsRTASBuilt()
	{
		return g_data->m_raytracerData.RtAS.GetTLAS().IsInitialized();
	}
}

//--------------------------------------------------------------------------------------
// DefaultRenderer
//--------------------------------------------------------------------------------------

Scene::Renderer::Interface DefaultRenderer::InitAndGetInterface()
{
	Assert(!g_data, "g_data has already been initialized.");
	g_data = new (std::nothrow) Data;

	Scene::Renderer::Interface rndIntrf;

	rndIntrf.Init = &DefaultRenderer::Init;
	rndIntrf.Update = &DefaultRenderer::Update;
	rndIntrf.Render = &DefaultRenderer::Render;
	rndIntrf.Shutdown = &DefaultRenderer::Shutdown;
	rndIntrf.OnWindowSizeChanged = &DefaultRenderer::OnWindowSizeChanged;
	rndIntrf.GetRenderGraph = &DefaultRenderer::GetRenderGraph;
	rndIntrf.DebugDrawRenderGraph = &DefaultRenderer::DebugDrawRenderGraph;
	rndIntrf.IsRTASBuilt = &DefaultRenderer::IsRTASBuilt;

	return rndIntrf;
}