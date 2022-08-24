#include "SceneRendererImpl.h"
#include "../Scene.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"
#include "../../Win32/Log.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/SharedShaderResources.h"
#include "../../SupportSystem/Task.h"
#include "../../Math/MatrixFuncs.h"


using namespace ZetaRay;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Win32;

namespace
{
	uint32_t g_i = 0;
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
	m_renderGraph.Reset();
	auto* data = m_data.get();

	Camera& cam = App::GetScene().GetCamera();
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
	data->m_frameConstants.SunDir.normalize();
	data->m_frameConstants.SunIlluminance = 40.0f;
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
	auto h0 = ts.EmplaceTask("GBufferRenderer_Init", [this]()
		{
			auto* data = m_data.get();
			GBufferRenderer::Init(data->m_settings, data->m_gBuffData);
		});
	auto h1 = ts.EmplaceTask("LightManager_Init", [this]()
		{
			auto* data = m_data.get();
			LightManager::Init(data->m_settings, data->m_lightManagerData);
		});
	auto h2 = ts.EmplaceTask("RayTracer_Init", [this]()
		{
			auto* data = m_data.get();
			RayTracer::Init(data->m_settings, data->m_raytracerData);
		});
	auto h3 = ts.EmplaceTask("PostProcessor_Init", [this]()
		{
			auto* data = m_data.get();
			PostProcessor::Init(data->m_settings, data->m_postProcessorData, data->m_lightManagerData);
		});

	ts.AddOutgoingEdge(h1, h3);
	ts.Sort();
	ts.Finalize();
	App::Submit(ZetaMove(ts));

	// render settings
	{
		ParamVariant enableTAA;
		enableTAA.InitBool("Renderer", "Settings", "TAA", fastdelegate::MakeDelegate(this, &SceneRenderer::SetTAAEnablement),
			false);
		App::AddParam(enableTAA);

		ParamVariant enableIndirectDiffuse;
		enableIndirectDiffuse.InitBool("Renderer", "Settings", "RaytracedIndirectDiffuse",
			fastdelegate::MakeDelegate(this, &SceneRenderer::SetIndirectDiffuseEnablement),
			m_data->m_settings.RTIndirectDiffuse);
		App::AddParam(enableIndirectDiffuse);

		ParamVariant enableDenoiser;
		enableDenoiser.InitBool("Renderer", "Settings", "IndirectDiffuseDenoiser",
			fastdelegate::MakeDelegate(this, &SceneRenderer::SetIndierctDiffuseDenoiserEnablement),
			m_data->m_settings.DenoiseIndirectDiffuseLi);
		App::AddParam(enableDenoiser);

		ParamVariant enableInscattering;
		enableInscattering.InitBool("Renderer", "Settings", "Inscattering",
			fastdelegate::MakeDelegate(this, &SceneRenderer::SetInscatteringEnablement),
			m_data->m_settings.Inscattering);
		App::AddParam(enableInscattering);

		//ParamVariant enableFSR2;
		//enableFSR2.InitBool("Renderer", "Settings", "FSR2",
		//	fastdelegate::MakeDelegate(this, &SceneRenderer::SetFsr2Enablement),
		//	m_data->m_settings.Fsr2);
		//App::AddParam(enableFSR2);

		ParamVariant p6;
		p6.InitEnum("Renderer", "Settings", "Upscaling", fastdelegate::MakeDelegate(this, &SceneRenderer::SetUpscalingMethod),
			UpscalingOptions, sizeof(UpscalingOptions) / sizeof(const char*), Upscaling::NATIVE);
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

			UpdateFrameConstants();

			auto* data = m_data.get();
			GBufferRenderer::Update(data->m_gBuffData, data->m_lightManagerData);
			LightManager::Update(data->m_settings, data->m_gBuffData, data->m_raytracerData, data->m_lightManagerData);
		});

	auto h1 = ts.EmplaceTask("SceneRenderer::Update_RtPost", [this]()
		{
			auto* data = m_data.get();

			// following order is important
			RayTracer::Update(data->m_settings, data->m_raytracerData);
			PostProcessor::Update(data->m_settings, data->m_gBuffData, data->m_lightManagerData, 
				data->m_raytracerData, data->m_postProcessorData);
		});

	auto h2 = ts.EmplaceTask("SceneRenderer::Update_Graph", [this]()
		{
			auto* data = m_data.get();
			m_renderGraph.BeginFrame();

			GBufferRenderer::Register(data->m_gBuffData, m_renderGraph);
			LightManager::Register(data->m_settings, data->m_raytracerData, data->m_lightManagerData, m_renderGraph);
			RayTracer::Register(data->m_settings, data->m_raytracerData, m_renderGraph);
			PostProcessor::Register(data->m_settings, data->m_postProcessorData, m_renderGraph);

			m_renderGraph.MoveToPostRegister();

			GBufferRenderer::DeclareAdjacencies(data->m_gBuffData, data->m_lightManagerData, m_renderGraph);
			LightManager::DeclareAdjacencies(data->m_settings, data->m_gBuffData, data->m_raytracerData,
				data->m_lightManagerData, m_renderGraph);
			RayTracer::DeclareAdjacencies(data->m_settings, data->m_gBuffData, data->m_raytracerData, m_renderGraph);
			PostProcessor::DeclareAdjacencies(data->m_settings, data->m_gBuffData, data->m_lightManagerData,
				data->m_raytracerData, data->m_postProcessorData, m_renderGraph);
		});

	// RenderGraph should update last
	ts.AddOutgoingEdge(h0, h1);
	ts.AddOutgoingEdge(h1, h2);
}

void SceneRenderer::Render(TaskSet& ts) noexcept
{
	m_renderGraph.Build(ts);
}

void SceneRenderer::Shutdown() noexcept
{
	GBufferRenderer::Shutdown(m_data->m_gBuffData);
	LightManager::Shutdown(m_data->m_lightManagerData);
	RayTracer::Shutdown(m_data->m_raytracerData);
	PostProcessor::Shutdown(m_data->m_postProcessorData);

	m_frameConstantsBuff.Reset();
	m_renderGraph.Shutdown();
}

void SceneRenderer::AddEmissiveInstance(uint64_t instanceID, Vector<float, 32>&& lumen) noexcept
{
	LightManager::AddEmissiveTriangle(m_data->m_lightManagerData, instanceID, ZetaMove(lumen));
}

void SceneRenderer::SetEnvLightSource(const Filesystem::Path& pathToEnvLight, const Filesystem::Path& pathToPatches) noexcept
{
	LightManager::SetEnvMap(m_data->m_lightManagerData, pathToEnvLight, pathToPatches);

	// fill the frame constants with the desc. heap idx
	m_data->m_frameConstants.EnvMapDescHeapOffset = m_data->m_lightManagerData.GpuDescTable.GPUDesciptorHeapIndex(
		LightManagerData::DESC_TABLE::ENV_MAP_SRV);
}

void SceneRenderer::UpdateFrameConstants() noexcept
{
	auto* data = m_data.get();

	const int currIdx = App::GetTimer().GetTotalFrameCount() & 0x1;
	//data->m_frameConstants.FrameNum = g_i++;
	//g_i++;
	data->m_frameConstants.FrameNum = (uint32_t)App::GetTimer().GetTotalFrameCount();
	//data->m_frameConstants.FrameNum = g_i++;

	//LOG("Frame: %u, g_i: %u\n", (uint32_t)App::GetTimer().GetTotalFrameCount(), g_i);

	data->m_frameConstants.RenderWidth = App::GetRenderer().GetRenderWidth();
	data->m_frameConstants.RenderHeight = App::GetRenderer().GetRenderHeight();
	data->m_frameConstants.DisplayWidth = App::GetRenderer().GetDisplayWidth();
	data->m_frameConstants.DisplayHeight = App::GetRenderer().GetDisplayHeight();
	data->m_frameConstants.MipBias = log2f((float)data->m_frameConstants.RenderWidth / data->m_frameConstants.DisplayWidth) - 1.0f;

	data->m_frameConstants.BaseColorMapsDescHeapOffset = App::GetScene().GetBaseColMapsDescHeapOffset();
	data->m_frameConstants.NormalMapsDescHeapOffset = App::GetScene().GetNormalMapsDescHeapOffset();
	data->m_frameConstants.MetalnessRoughnessMapsDescHeapOffset = App::GetScene().GetMetallicRougnessMapsDescHeapOffset();
	data->m_frameConstants.EmissiveMapsDescHeapOffset = App::GetScene().GetEmissiveMapsDescHeapOffset();

	// Note: assumes BVH has been built
	data->m_frameConstants.WorldRadius = App::GetScene().GetWorldAABB().Extents.length();

	// camera
	Camera& cam = App::GetScene().GetCamera();
	v_float4x4 vCurrV(const_cast<float4x4a&>(cam.GetCurrView()));
	v_float4x4 vP(const_cast<float4x4a&>(cam.GetCurrProj()));
	v_float4x4 vVP = mul(vCurrV, vP);

	data->m_frameConstants.CameraPos = cam.GetPos();
	data->m_frameConstants.CameraNear = cam.GetNearZ();
	data->m_frameConstants.CameraFar = cam.GetFarZ();
	data->m_frameConstants.AspectRatio = cam.GetAspectRatio();
	data->m_frameConstants.PixelSpreadAngle = cam.GetPixelSpreadAngle();
	data->m_frameConstants.TanHalfFOV = tanf(0.5f * cam.GetFOV());
	data->m_frameConstants.CurrProj = cam.GetCurrProj();
	data->m_frameConstants.PrevView = data->m_frameConstants.CurrView;
	data->m_frameConstants.CurrView = float3x4(cam.GetCurrView());
	data->m_frameConstants.PrevViewProj = m_data->m_frameConstants.CurrViewProj;
	data->m_frameConstants.CurrViewProj = store(vVP);
	data->m_frameConstants.PrevViewInv = m_data->m_frameConstants.CurrViewInv;
	data->m_frameConstants.CurrViewInv = float3x4(cam.GetViewInv());
	data->m_frameConstants.PrevCameraJitter = data->m_frameConstants.CurrCameraJitter;
	data->m_frameConstants.CurrCameraJitter = cam.GetProjOffset();

	// frame gbuffer srv desc. table
	data->m_frameConstants.CurrGBufferDescHeapOffset = m_data->m_gBuffData.SRVDescTable[currIdx].GPUDesciptorHeapIndex();
	data->m_frameConstants.PrevGBufferDescHeapOffset = m_data->m_gBuffData.SRVDescTable[1 - currIdx].GPUDesciptorHeapIndex();

	// env. map SRV
	m_data->m_frameConstants.EnvMapDescHeapOffset = m_data->m_lightManagerData.GpuDescTable.GPUDesciptorHeapIndex(
		LightManagerData::DESC_TABLE::ENV_MAP_SRV);

	m_frameConstantsBuff = App::GetRenderer().GetGpuMemory().GetDefaultHeapBufferAndInit(FRAME_CONSTANTS_BUFFER_NAME,
		Math::AlignUp(sizeof(cbFrameConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), 
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		false, 
		&data->m_frameConstants);

	App::GetRenderer().GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(FRAME_CONSTANTS_BUFFER_NAME, 
		m_frameConstantsBuff);
}

void SceneRenderer::OnWindowSizeChanged() noexcept
{
	auto* data = m_data.get();

	// following order is important
	GBufferRenderer::OnWindowSizeChanged(data->m_settings, data->m_gBuffData);
	LightManager::OnWindowSizeChanged(data->m_settings, data->m_lightManagerData);
	RayTracer::OnWindowSizeChanged(data->m_settings, data->m_raytracerData);
	PostProcessor::OnWindowSizeChanged(data->m_settings, data->m_postProcessorData, data->m_lightManagerData);

	m_renderGraph.Reset();
}

void SceneRenderer::SetTAAEnablement(const ParamVariant& p) noexcept
{
	m_data->m_settings.TAA = p.GetBool();
}

void SceneRenderer::SetIndirectDiffuseEnablement(const ParamVariant& p) noexcept
{
	m_data->m_settings.RTIndirectDiffuse = p.GetBool();
}

void SceneRenderer::SetIndierctDiffuseDenoiserEnablement(const ParamVariant& p) noexcept
{
	m_data->m_settings.DenoiseIndirectDiffuseLi = m_data->m_settings.RTIndirectDiffuse && p.GetBool();
}

void SceneRenderer::SetInscatteringEnablement(const ParamVariant& p) noexcept
{
	m_data->m_settings.Inscattering = p.GetBool();
}

void SceneRenderer::SetUpscalingMethod(const ParamVariant& p) noexcept
{
	int u = p.GetEnum().m_curr;

	if (u == Upscaling::POINT)
	{
		// following order is important
		m_data->m_settings.Fsr2 = false;

		if (m_data->m_postProcessorData.Fsr2Pass.IsInitialized())
			m_data->m_postProcessorData.Fsr2Pass.Reset();

		App::SetUpscalingEnablement(true);
	}
	else if (u == Upscaling::FSR2)
	{
		// following order is important
		App::SetUpscalingEnablement(true);
		m_data->m_settings.Fsr2 = true;
	}
	else if (u == Upscaling::NATIVE)
	{
		// following order is important
		m_data->m_settings.Fsr2 = false;

		if (m_data->m_postProcessorData.Fsr2Pass.IsInitialized())
			m_data->m_postProcessorData.Fsr2Pass.Reset();

		App::SetUpscalingEnablement(false);
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
