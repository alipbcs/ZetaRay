#include "../Common/Common.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/VolumetricLighting.hlsli"

#define USE_ENVIRONMENT_MAP 1
#define NON_LINEAR_LATITUDE 1

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0, space0);

//--------------------------------------------------------------------------------------
// Helper structs
//--------------------------------------------------------------------------------------

struct VSIn
{
	float3 PosL : POSITION;
	float3 NormalL : NORMAL;
	float2 TexUV : TEXUV;
	float3 TangentU : TANGENT;
};

struct VSOut
{
	float4 PosSS : SV_Position;
	float3 PosW : POSW;
};

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

VSOut mainVS(VSIn vsin)
{
	VSOut vsout;

	// transform to view-space so that sphere is aligned with the view axes		
	float3 posV = mul((float3x3) g_frame.CurrView, vsin.PosL);
	vsout.PosSS = mul(float4(posV, 1.0f), g_frame.CurrProj);
	// force z value to be on the far plane
	vsout.PosSS.z = 0.0f;
	
	vsout.PosW = vsin.PosL;

	return vsout;
}

float4 mainPS(VSOut psin) : SV_Target
{
//	float3 w = normalize(psin.PosW);
//	float3 w = normalize(psin.PosW - g_frame.CameraPos);
	
	// skydome center
	float3 center = float3(0, 1e-1, 0);
	
	float3 w = normalize(psin.PosW - center);
	const float3 sigma_s_rayleigh = g_frame.RayleighSigmaSColor * g_frame.RayleighSigmaSScale;
	const float sigma_t_mie = g_frame.MieSigmaA + g_frame.MieSigmaS;
	const float3 sigma_a_ozone = g_frame.OzoneSigmaAColor * g_frame.OzoneSigmaAScale;
	float3 rayOrigin = center;
	rayOrigin.y += g_frame.PlanetRadius;
	float3 color = 0.0f.xxx;

	// a circle that is supposed to be sun
	if (dot(-w, g_frame.SunDir) >= g_frame.SunCosAngularRadius)
	{
		float t;
		bool intersectedPlanet = Volumetric::IntersectRayPlanet(g_frame.PlanetRadius, rayOrigin, w, t);
		
		if (!intersectedPlanet)
			color = g_frame.SunIlluminance;
	}
	// sample the sky texture otherwise
	else
	{
#if USE_ENVIRONMENT_MAP == 1
		// x = sin(theta) * cos(phi)
		// y = cos(theta)
		// z = sin(theta) * sin(phi)
		float phi = atan2(w.z, w.x); // [-PI, PI]
		phi += PI;
//		const float theta = Common::ArcCos(w.y); // [0, PI]
//		float2 thetaPhi = float2(theta, phi);
		
		float2 thetaPhi = Math::SphericalFromCartesian(w);
		thetaPhi.y = phi;
		
		const float u = thetaPhi.y * ONE_DIV_TWO_PI;
		float v = thetaPhi.x * ONE_DIV_PI;
		
#if NON_LINEAR_LATITUDE == 1				
		float s = thetaPhi.x >= PI_DIV_2 ? 1.0f : -1.0f;
		v = (thetaPhi.x - PI_DIV_2) * 0.5f;
		v = 0.5f + s * sqrt(abs(v) * ONE_DIV_PI);
#endif
		
		Texture2D<half3> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
		color = g_envMap.SampleLevel(g_samLinearClamp, float2(u, v), 0.0f);		
#else
		
		// in-scattered lighting
		float3 Ls = Volumetric::EstimateLs(g_frame.PlanetRadius, rayOrigin, w, g_frame.SunDir, g_frame.AtmosphereAltitude, g_frame.g,
			sigma_s_rayleigh, g_frame.MieSigmaS, sigma_t_mie, sigma_a_ozone, 32);
	
		color = Ls * g_frame.SunIlluminance;
#endif
	}
	
	return float4(color, 1.0f);
}
