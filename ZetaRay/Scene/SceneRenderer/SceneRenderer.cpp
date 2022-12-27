#include "SceneRendererImpl.h"
#include "../SceneCore.h"
#include "../../App/Timer.h"
#include "../../App/Log.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/SharedShaderResources.h"
#include "../../Support/Task.h"
#include "../../Math/MatrixFuncs.h"

using namespace ZetaRay::Scene;
using namespace ZetaRay::Scene::Settings;
using namespace ZetaRay::Scene::Render;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// Scene::Render::Common
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

	frameConstsBuff = App::GetRenderer().GetGpuMemory().GetDefaultHeapBufferAndInit(Scene::SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME,
		Math::AlignUp(sizeof(cbFrameConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		false,
		&frameConsts);

	App::GetRenderer().GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Scene::SceneRenderer::FRAME_CONSTANTS_BUFFER_NAME,
		frameConstsBuff);
}

//--------------------------------------------------------------------------------------
// SceneRenderer
//--------------------------------------------------------------------------------------

SceneRenderer::SceneRenderer() noexcept
{
	m_data.reset(new(std::nothrow) Data);
}

SceneRenderer::~SceneRenderer() noexcept
{
}

void SceneRenderer::Init() noexcept
{
	auto* data = m_data.get();
	data->m_renderGraph.Reset();

	const Camera& cam = App::GetCamera();
	v_float4x4 vCurrV(const_cast<float4x4a&>(cam.GetCurrView()));
	v_float4x4 vP(const_cast<float4x4a&>(cam.GetCurrProj()));
	v_float4x4 vVP = mul(vCurrV, vP);

	// for 1st frame
	//data->m_frameConstants.UseReverseZ = RendererConstants::USE_REVERSE_Z;
	data->m_frameConstants.PrevViewProj = store(vVP);
	data->m_frameConstants.PrevViewInv = float3x4(cam.GetViewInv());
	data->m_frameConstants.PrevView = float3x4(cam.GetCurrView());
	data->m_frameConstants.RayOffset = 3e-2f;

	data->m_frameConstants.SunDir = float3(0.223f, -0.96f, -0.167f);
	//data->m_frameConstants.SunDir = float3(0.6169695854187012, -0.6370740532875061, -0.4620445668697357);
	data->m_frameConstants.SunDir.normalize();
	//data->m_frameConstants.SunIlluminance = 50.0f;
	data->m_frameConstants.SunIlluminance = 187.0f;
	// sun angular diamter ~ 0.545 degrees 
	// 0.5 degrees == 0.0087266 radians 
	// cos(0.0087266 / 2)
	data->m_frameConstants.SunCosAngularRadius = 0.99998869f;
	data->m_frameConstants.AtmosphereAltitude = Defaults::ATMOSPHERE_ALTITUDE;
	data->m_frameConstants.PlanetRadius = Defaults::PLANET_RADIUS;
	data->m_frameConstants.g = Defaults::g;

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

	normalizeAndStore(Defaults::SIGMA_S_RAYLEIGH, data->m_frameConstants.RayleighSigmaSColor, data->m_frameConstants.RayleighSigmaSScale);
	normalizeAndStore(float3(Defaults::SIGMA_A_OZONE), data->m_frameConstants.OzoneSigmaAColor, data->m_frameConstants.OzoneSigmaAScale);

	data->m_frameConstants.MieSigmaA = Defaults::SIGMA_A_MIE;
	data->m_frameConstants.MieSigmaS = Defaults::SIGMA_S_MIE;

	TaskSet ts;
	auto h0 = ts.EmplaceTask("GBuffer_Init", [this]()
		{
			auto* data = m_data.get();
			GBuffer::Init(data->m_settings, data->m_gBuffData);
		});
	auto h1 = ts.EmplaceTask("Light_Init", [this]()
		{
			auto* data = m_data.get();
			Light::Init(data->m_settings, data->m_lightData);
		});
	auto h2 = ts.EmplaceTask("RayTracer_Init", [this]()
		{
			auto* data = m_data.get();
			RayTracer::Init(data->m_settings, data->m_raytracerData);
		});
	auto h3 = ts.EmplaceTask("PostProcessor_Init", [this]()
		{
			auto* data = m_data.get();
			PostProcessor::Init(data->m_settings, data->m_postProcessorData, data->m_lightData);
		});

	ts.AddOutgoingEdge(h1, h3);
	ts.Sort();
	ts.Finalize();
	App::Submit(ZetaMove(ts));

	// render settings
	{
		ParamVariant enableIndirectDiffuse;
		enableIndirectDiffuse.InitBool("Renderer", "Settings", "RaytracedIndirectDiffuse",
			fastdelegate::MakeDelegate(this, &SceneRenderer::SetIndirectDiffuseEnablement),
			m_data->m_settings.RTIndirectDiffuse);
		App::AddParam(enableIndirectDiffuse);

		//ParamVariant enableDenoiser;
		//enableDenoiser.InitEnum("Renderer", "Settings", "IndirectDiffuseDenoiser",
		//	fastdelegate::MakeDelegate(this, &SceneRenderer::SetIndierctDiffuseDenoiser),
		//	Denoisers, ZetaArrayLen(Denoisers), (int)DENOISER::STAD);
		//App::AddParam(enableDenoiser);

		ParamVariant enableInscattering;
		enableInscattering.InitBool("Renderer", "Settings", "Inscattering",
			fastdelegate::MakeDelegate(this, &SceneRenderer::SetInscatteringEnablement),
			m_data->m_settings.Inscattering);
		App::AddParam(enableInscattering);

		ParamVariant p6;
		p6.InitEnum("Renderer", "Settings", "Upscaling/AA", fastdelegate::MakeDelegate(this, &SceneRenderer::SetAA),
			AAOptions, ZetaArrayLen(AAOptions), (int) AA::NATIVE);
		App::AddParam(p6);
	}

	ParamVariant rayOffset;
	rayOffset.InitFloat("Renderer", "General", "RayOffset", fastdelegate::MakeDelegate(this, &SceneRenderer::RayOffset),
		Defaults::RAY_T_OFFSET, 
		1e-4f,
		1e-1f, 
		1e-4f);
	App::AddParam(rayOffset);

	// sun
	{
		ParamVariant p0;
		p0.InitUnitDir("LightSource", "Sun", "(-)Dir",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifySunDir),
			-data->m_frameConstants.SunDir);
		App::AddParam(p0);

		ParamVariant p2;
		p2.InitFloat("LightSource", "Sun", "Illuminance",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifySunLux),
			data->m_frameConstants.SunIlluminance,
			1.0f,
			1000.0f,
			1.0f);
		App::AddParam(p2);

		ParamVariant p3;
		p3.InitFloat("LightSource", "Sun", "AngularRadius",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifySunAngularRadius),
			acosf(data->m_frameConstants.SunCosAngularRadius),
			0.004f,
			0.02f,
			1e-3f);
		App::AddParam(p3);
	}

	// atmosphere
	{
		ParamVariant p0;
		p0.InitColor("Scene", "Atmosphere", "Rayleigh scattering color",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifyRayleighSigmaSColor),
			data->m_frameConstants.RayleighSigmaSColor);
		App::AddParam(p0);

		ParamVariant p1;
		p1.InitFloat("Scene", "Atmosphere", "Rayleigh scattering scale",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifyRayleighSigmaSScale),
			data->m_frameConstants.RayleighSigmaSScale, 
			0.0f, 
			10.0f, 
			3.0f);
		App::AddParam(p1);

		ParamVariant p2;
		p2.InitFloat("Scene", "Atmosphere", "Mie scattering coeff.",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifyMieSigmaS),
			Defaults::SIGMA_S_MIE,
			1e-6f,
			1e-1f,
			1e-3f);
		App::AddParam(p2);

		ParamVariant p3;
		p3.InitFloat("Scene", "Atmosphere", "Mie absorption coeff.",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifyMieSigmaA),
			Defaults::SIGMA_A_MIE,
			1e-6f,
			10.0f,
			1e-3f);
		App::AddParam(p3);

		ParamVariant p4;
		p4.InitFloat("Scene", "Atmosphere", "Ozone absorption scale",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifyOzoneSigmaAScale),
			data->m_frameConstants.OzoneSigmaAScale,
			0.0f,
			10.0f,
			3.0f);
		App::AddParam(p4);

		ParamVariant p6;
		p6.InitColor("Scene", "Atmosphere", "Ozone absorption color",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifyOzoneSigmaAColor),
			data->m_frameConstants.OzoneSigmaAColor);
		App::AddParam(p6);

		ParamVariant p7;
		p7.InitFloat("Scene", "Atmosphere", "g (HG Phase Function)",
			fastdelegate::MakeDelegate(this, &SceneRenderer::ModifygForPhaseHG),
			Defaults::g,
			-0.99f,
			0.99f,
			0.2f);
		App::AddParam(p7);
	}
}

void SceneRenderer::Update(TaskSet& ts) noexcept
{
	auto h0 = ts.EmplaceTask("SceneRenderer::Update_GBuffLight", [this]()
		{
			auto* data = m_data.get();
			GBuffer::Update(data->m_gBuffData, data->m_lightData);

			Common::UpdateFrameConstants(data->m_frameConstants, data->m_frameConstantsBuff, data->m_gBuffData, data->m_lightData);
		});

	auto h1 = ts.EmplaceTask("SceneRenderer::Update_RT", [this]()
		{
			auto* data = m_data.get();
			RayTracer::Update(data->m_settings, data->m_raytracerData);
			Light::Update(data->m_settings, data->m_gBuffData, data->m_raytracerData, data->m_lightData);
		});

	auto h2 = ts.EmplaceTask("SceneRenderer::Update_RT_Graph", [this]()
		{
			auto* data = m_data.get();

			PostProcessor::Update(data->m_settings, data->m_gBuffData, data->m_lightData,
				data->m_raytracerData, data->m_postProcessorData);
		
			data->m_renderGraph.BeginFrame();

			GBuffer::Register(data->m_gBuffData, data->m_renderGraph);
			Light::Register(data->m_settings, data->m_raytracerData, data->m_lightData, data->m_renderGraph);
			RayTracer::Register(data->m_settings, data->m_raytracerData, data->m_renderGraph);
			PostProcessor::Register(data->m_settings, data->m_postProcessorData, data->m_renderGraph);

			data->m_renderGraph.MoveToPostRegister();

			GBuffer::DeclareAdjacencies(data->m_gBuffData, data->m_lightData, data->m_renderGraph);
			Light::DeclareAdjacencies(data->m_settings, data->m_gBuffData, data->m_raytracerData,
				data->m_lightData, data->m_renderGraph);
			RayTracer::DeclareAdjacencies(data->m_settings, data->m_gBuffData, data->m_raytracerData, data->m_renderGraph);
			PostProcessor::DeclareAdjacencies(data->m_settings, data->m_gBuffData, data->m_lightData,
				data->m_raytracerData, data->m_postProcessorData, data->m_renderGraph);
		});

	// RenderGraph should update last
	ts.AddOutgoingEdge(h0, h2);
	ts.AddOutgoingEdge(h1, h2);
}

void SceneRenderer::Render(TaskSet& ts) noexcept
{
	m_data->m_renderGraph.Build(ts);
}

void SceneRenderer::Shutdown() noexcept
{
	GBuffer::Shutdown(m_data->m_gBuffData);
	Light::Shutdown(m_data->m_lightData);
	RayTracer::Shutdown(m_data->m_raytracerData);
	PostProcessor::Shutdown(m_data->m_postProcessorData);

	m_data->m_frameConstantsBuff.Reset();
	m_data->m_renderGraph.Shutdown();
}

void SceneRenderer::OnWindowSizeChanged() noexcept
{
	auto* data = m_data.get();

	// following order is important
	GBuffer::OnWindowSizeChanged(data->m_settings, data->m_gBuffData);
	Light::OnWindowSizeChanged(data->m_settings, data->m_lightData);
	RayTracer::OnWindowSizeChanged(data->m_settings, data->m_raytracerData);
	PostProcessor::OnWindowSizeChanged(data->m_settings, data->m_postProcessorData, data->m_lightData);

	m_data->m_renderGraph.Reset();
}

void SceneRenderer::DebugDrawRenderGraph() noexcept
{
	m_data->m_renderGraph.DebugDrawGraph();
}

void SceneRenderer::SetIndirectDiffuseEnablement(const ParamVariant& p) noexcept
{
	m_data->m_settings.RTIndirectDiffuse = p.GetBool();
}

void SceneRenderer::SetIndierctDiffuseDenoiser(const ParamVariant& p) noexcept
{
	const int e = p.GetEnum().m_curr;
	Assert(e < (int)DENOISER::COUNT, "invalid enum value");
	const DENOISER u = (DENOISER) e;

	if (u == DENOISER::NONE)
		m_data->m_settings.IndirectDiffuseDenoiser = DENOISER::NONE;
	else if (u == DENOISER::STAD)
		m_data->m_settings.IndirectDiffuseDenoiser = m_data->m_settings.RTIndirectDiffuse ? DENOISER::STAD : DENOISER::NONE;
}

void SceneRenderer::SetInscatteringEnablement(const ParamVariant& p) noexcept
{
	m_data->m_settings.Inscattering = p.GetBool();
}

void SceneRenderer::SetAA(const ParamVariant& p) noexcept
{
	const int e = p.GetEnum().m_curr;
	Assert(e < (int)AA::COUNT, "invalid enum value");
	const AA u = (AA)e;

	if (u == AA::NATIVE)
	{
		// following order is important
		m_data->m_settings.AntiAliasing = AA::NATIVE;

		if (m_data->m_postProcessorData.Fsr2Pass.IsInitialized())
			m_data->m_postProcessorData.Fsr2Pass.Reset();

		if (m_data->m_postProcessorData.TaaPass.IsInitialized())
			m_data->m_postProcessorData.TaaPass.Reset();

		App::SetUpscalingEnablement(false);
	}
	else if (u == AA::POINT)
	{
		// following order is important
		m_data->m_settings.AntiAliasing = AA::POINT;

		if (m_data->m_postProcessorData.Fsr2Pass.IsInitialized())
			m_data->m_postProcessorData.Fsr2Pass.Reset();

		if (m_data->m_postProcessorData.TaaPass.IsInitialized())
			m_data->m_postProcessorData.TaaPass.Reset();

		App::SetUpscalingEnablement(true);
	}
	else if (u == AA::FSR2)
	{
		// following order is important
		if (m_data->m_postProcessorData.TaaPass.IsInitialized())
			m_data->m_postProcessorData.TaaPass.Reset();

		App::SetUpscalingEnablement(true);
		m_data->m_settings.AntiAliasing = AA::FSR2;
	}
	else if (u == AA::NATIVE_TAA)
	{
		// following order is important
		if (m_data->m_postProcessorData.Fsr2Pass.IsInitialized())
			m_data->m_postProcessorData.Fsr2Pass.Reset();

		App::SetUpscalingEnablement(false);
		m_data->m_settings.AntiAliasing = AA::NATIVE_TAA;
	}
}

void SceneRenderer::RayOffset(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.RayOffset = p.GetFloat().m_val;
}

void SceneRenderer::ModifySunDir(const ParamVariant& p) noexcept
{
	float pitch = p.GetUnitDir().m_pitch;
	float yaw = p.GetUnitDir().m_yaw;
	m_data->m_frameConstants.SunDir = -Math::SphericalToCartesian(pitch, yaw);
}

void SceneRenderer::ModifySunLux(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.SunIlluminance = p.GetFloat().m_val;
}

void SceneRenderer::ModifySunAngularRadius(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.SunCosAngularRadius = cosf(p.GetFloat().m_val);
}

void SceneRenderer::ModifyRayleighSigmaSColor(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.RayleighSigmaSColor = p.GetFloat3().m_val;
}

void SceneRenderer::ModifyRayleighSigmaSScale(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.RayleighSigmaSScale = p.GetFloat().m_val;
}

void SceneRenderer::ModifyMieSigmaS(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.MieSigmaS = p.GetFloat().m_val;
}

void SceneRenderer::ModifyMieSigmaA(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.MieSigmaA = p.GetFloat().m_val;
}

void SceneRenderer::ModifyOzoneSigmaAColor(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.OzoneSigmaAColor = p.GetColor().m_val;
}

void SceneRenderer::ModifyOzoneSigmaAScale(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.OzoneSigmaAScale = p.GetFloat().m_val;
}

void SceneRenderer::ModifygForPhaseHG(const ParamVariant& p) noexcept
{
	m_data->m_frameConstants.g = p.GetFloat().m_val;
}
