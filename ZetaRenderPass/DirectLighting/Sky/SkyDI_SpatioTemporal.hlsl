#include "SkyDI_Reservoir.hlsli"
#include "../../Common/Common.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/BRDF.hlsli"
#include "../../Common/Sampling.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"
#include "../../Common/RT.hlsli"
#include "../../Common/Volumetric.hlsli"

#define TARGET_WITH_VISIBILITY 1
#define THREAD_GROUP_SWIZZLING 1
#define MIN_ROUGHNESS_SURFACE_MOTION 0.4
#define MAX_NUM_TEMPORAL_SAMPLES 4
#define MAX_PLANE_DIST_REUSE 0.0025
#define MIN_NORMAL_SIMILARITY_REUSE 0.906307787	// within 25 degrees
#define MAX_ROUGHNESS_DIFF_REUSE 0.1f
#define NUM_SPATIAL_SAMPLES 2
#define SPATIAL_SEARCH_RADIUS 32

struct TemporalCandidate
{
	static TemporalCandidate Init()
	{
		TemporalCandidate ret;
		ret.valid = false;

		return ret;
	}

	float3 posW;
	float roughness;
	float3 normal;
	int16_t2 posSS;
	bool valid;
};

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cbFrameConstants> g_frame : register(b0);
ConstantBuffer<cb_SkyDI_Temporal> g_local : register(b1);
RaytracingAccelerationStructure g_bvh : register(t0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

bool Visibility(float3 pos, float3 wi, float3 normal, float linearDepth)
{
	if(wi.y < 0)
		return false;

	float tMin;
	const float3 adjustedOrigin = RT::OffsetRay(pos, normal, linearDepth, tMin);

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
		RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
		RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
		RAY_FLAG_FORCE_OPAQUE> rayQuery;

	RayDesc ray;
	ray.Origin = adjustedOrigin;
	ray.TMin = tMin;
	ray.TMax = FLT_MAX;
	ray.Direction = wi;

	// Initialize
	rayQuery.TraceRayInline(g_bvh, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);

	// Traversal
	rayQuery.Proceed();

	// Light source is occluded
	if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		return false;

	return true;
}

float3 Target(float3 pos, float3 normal, float linearDepth, float3 wi, BRDF::SurfaceInteraction surface, 
	out float3 Lo)
{
#if TARGET_WITH_VISIBILITY
	const bool vis = Visibility(pos, wi, normal, linearDepth);
	if (!vis)
		return 0.0;
#endif

	// sample sky-view LUT
	Texture2D<float4> g_envMap = ResourceDescriptorHeap[g_frame.EnvMapDescHeapOffset];
	const float2 thetaPhi = Math::SphericalFromCartesian(wi);
	float2 uv = float2(thetaPhi.y * ONE_OVER_2_PI, thetaPhi.x * ONE_OVER_PI);

	// undo non-linear sampling
	const float s = thetaPhi.x >= PI_OVER_2 ? 1.0f : -1.0f;
	uv.y = (thetaPhi.x - PI_OVER_2) * 0.5f;
	uv.y = 0.5f + s * sqrt(abs(uv.y) * ONE_OVER_PI);
	
	Lo = g_envMap.SampleLevel(g_samLinearClamp, uv, 0.0f).rgb;
		
	const float3 brdfCosTheta = BRDF::SurfaceBRDF(surface, true);
	const float3 target = Lo * brdfCosTheta;

	return target;
}

// Jacobian of the mapping r = T(q), e.g. reusing a path from pixel q at pixel r
//		T[x1_q, x2_q, x3_q, ...] = x1_r, x2_q, x3_q, ...
float JacobianReconnectionShift(float3 x1_q, float3 wi_q, float3 x1_r)
{
	if(all(wi_q) == 0)
		return 0;

	x1_q.y += g_frame.PlanetRadius;
	x1_r.y += g_frame.PlanetRadius;

	float3 x2_q = g_frame.AtmosphereAltitude * wi_q;
	float3 v_q = x1_q - x2_q;
	float t_q2 = dot(v_q, v_q);
	v_q = all(v_q == 0) ? v_q : v_q / sqrt(t_q2);
	float3 x2_q_normal = -wi_q;

	float3 v_r = x1_r - x2_q;
	const float t_r2 = dot(v_r, v_r);
	v_r = all(v_r == 0) ? v_r : v_r / sqrt(t_r2);

	//  - phi_r is the angle between x1_r - x2_q and surface normal at x2_q
	//  - phi_q is the angle between x1_q - x2_q and surface normal at x2_q
	float cosPhi_r = dot(v_r, x2_q_normal);
	float cosPhi_q = dot(v_q, x2_q_normal);
	return (abs(cosPhi_r) * t_q2) / max(abs(cosPhi_q) * t_r2, 1e-6);
}

// Ref: Bitterli, Benedikt, "Correlations and Reuse for Fast and Accurate Physically Based Light Transport" (2022). Ph.D Dissertation.
// https://digitalcommons.dartmouth.edu/dissertations/77
struct PairwiseMIS
{	
	static PairwiseMIS Init(uint16_t numStrategies, SkyDI_Util::Reservoir r_c)
	{
		PairwiseMIS ret;

		ret.r_s = SkyDI_Util::Reservoir::Init();
		ret.m_c = 1.0f;
		ret.M_s = r_c.M;
		ret.k = numStrategies;

		return ret;
	}

	float Compute_m_i(SkyDI_Util::Reservoir r_c, float targetLum, SkyDI_Util::Reservoir r_i, 
		float w_sum_i, float jacobianNeighborToCurr)
	{
		// TODO following seems to be the correct term to use, but for some reason gives terrible results
#if 0
		const float p_i_y_i = r_i.W > 0 ? w_sum_i / r_i.W : 0;
#else
		const float p_i_y_i = r_i.W > 0 ? (r_i.M * w_sum_i) / r_i.W : 0;
#endif

		const float p_c_y_i = targetLum * r_c.M;
		// Jacobian term in the numerator cancels out with the same term in the resampling weight
		float numerator = r_i.M * p_i_y_i;
		float denom = numerator / jacobianNeighborToCurr + (r_c.M / this.k) * p_c_y_i;
		float m_i = denom > 0 ? numerator / denom : 0;

		return m_i;
	}

	void Update_m_c(SkyDI_Util::Reservoir r_c, SkyDI_Util::Reservoir r_i, float3 brdfCosTheta_i, 
		float jacobianCurrToNeighbor)
	{
		if(!r_i.IsValid())
		{
			this.m_c += 1;
			return;
		}

		const float target_i = Math::Color::LuminanceFromLinearRGB(r_c.Le * brdfCosTheta_i);
		const float p_i_y_c = target_i * jacobianCurrToNeighbor;

		const float p_c_y_c = Math::Color::LuminanceFromLinearRGB(r_c.Target);

		const float numerator = r_i.M * p_i_y_c;
		const bool denomGt0 = (p_c_y_c + numerator) > 0; 
		this.m_c += denomGt0 ? 1 - numerator / (numerator + (r_c.M / this.k) * p_c_y_c) : 1;
	}

	void Stream(SkyDI_Util::Reservoir r_c, float3 posW_c, float3 normal_c, float linearDepth_c, 
		BRDF::SurfaceInteraction surface_c, SkyDI_Util::Reservoir r_i, float3 posW_i, float3 normal_i, 
		float w_sum_i, BRDF::SurfaceInteraction surface_i, inout RNG rng)
	{
		float3 currTarget;
		float m_i;

		// m_i
		if(r_i.IsValid())
		{
			surface_c.SetWi(r_i.wi, normal_c);
			const float3 brdfCosTheta_c = BRDF::SurfaceBRDF(surface_c);
			currTarget = r_i.Le * brdfCosTheta_c;

#if TARGET_WITH_VISIBILITY == 1
			if(Math::Color::LuminanceFromLinearRGB(currTarget) > 1e-5)
				currTarget *= Visibility(posW_c, r_i.wi, normal_c, linearDepth_c);
#endif

			const float targetLum = Math::Color::LuminanceFromLinearRGB(currTarget);
			const float J_temporal_to_curr = JacobianReconnectionShift(posW_i, r_i.wi, posW_c);
			m_i = Compute_m_i(r_c, targetLum, r_i, w_sum_i, J_temporal_to_curr);
		}

		float3 brdfCosTheta_i;
		float J_curr_to_temporal;

		// m_c
		if(r_c.IsValid())
		{
			surface_i.SetWi(r_c.wi, normal_i);
			brdfCosTheta_i = BRDF::SurfaceBRDF(surface_i);

#if TARGET_WITH_VISIBILITY == 1
			if(Math::Color::LuminanceFromLinearRGB(brdfCosTheta_i) > 1e-5)
				brdfCosTheta_i *= Visibility(posW_i, r_c.wi, normal_i, linearDepth_c);
#endif

			J_curr_to_temporal = JacobianReconnectionShift(posW_c, r_c.wi, posW_i);
		}	

		Update_m_c(r_c, r_i, brdfCosTheta_i, J_curr_to_temporal);

		if(r_i.IsValid())
		{
			// Jacobian term cancels out with the same term in m_i's numerator
			const float w_i = m_i * Math::Color::LuminanceFromLinearRGB(currTarget) * r_i.W;

			if (this.r_s.Update(w_i, r_i.Le, r_i.wi, currTarget, rng))
				this.r_s.NeedsShadowRay = 1 - TARGET_WITH_VISIBILITY;
		}

		this.M_s += r_i.M;
	}

	void End(SkyDI_Util::Reservoir r_c, inout RNG rng)
	{
		const float w_c = Math::Color::LuminanceFromLinearRGB(r_c.Target) * r_c.W * this.m_c;

		if(this.r_s.Update(w_c, r_c.Le, r_c.wi, r_c.Target, rng))
			this.r_s.NeedsShadowRay = this.r_s.NeedsShadowRay || r_c.NeedsShadowRay;

		this.r_s.M = this.M_s;
		const float targetLum = Math::Color::LuminanceFromLinearRGB(r_s.Target);
		this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
		// TODO investigate
		this.r_s.W = isnan(this.r_s.W) ? 0 : this.r_s.W;
	}

	SkyDI_Util::Reservoir r_s;
	float m_c;
	half M_s;
	uint16_t k;
	float p_hat_c;
};

SkyDI_Util::Reservoir RIS_InitialCandidates(uint2 DTid, float3 posW, float3 normal, float linearDepth, bool metallic,
	float roughness, BRDF::SurfaceInteraction surface, inout RNG rng)
{
	SkyDI_Util::Reservoir r = SkyDI_Util::Reservoir::Init();

	// MIS -- take a diffuse/hemisphere sample and a brdf sample

	// sample diffuse BRDF for dielectrics, hemisphere for metals
	if(!metallic || roughness > g_local.MinRoughnessResample)
	{
		const float2 u = rng.Uniform2D();
		float p_d;
		float3 wi_d;

		if(!metallic)
			wi_d = BRDF::SampleLambertianBrdf(normal, u, p_d);
		else
		{
			wi_d = Sampling::SampleCosineWeightedHemisphere(u, p_d);
			wi_d = float3(wi_d.x, wi_d.z, -wi_d.y);
		}

		surface.SetWi(wi_d, normal);

		float3 Lo;
		const float3 target = Target(posW, normal, linearDepth, wi_d, surface, Lo);

		// balance heuristic
		// p_d in m_d's numerator and w_d's denominator cancel out
		const float m_d = 1.0f / max(p_d + BRDF::SpecularBRDFGGXSmithPdf(surface), 1e-6);
		const float w_d = m_d * Math::Color::LuminanceFromLinearRGB(target);

		if(r.Update(w_d, Lo, wi_d, target, rng))
			r.NeedsShadowRay = 1 - TARGET_WITH_VISIBILITY;
	}

	// sample specular BRDF
	{
		const float2 u = rng.Uniform2D();
		const float3 wi_s = BRDF::SampleSpecularBRDFGGXSmith(surface, normal, u);
		surface.SetWi(wi_s, normal);
		const float p_s = BRDF::SpecularBRDFGGXSmithPdf(surface);

		float3 Lo;
		const float3 target = Target(posW, normal, linearDepth, wi_s, surface, Lo);
		const float p_d = (metallic && roughness < g_local.MinRoughnessResample) ? 
			0 : 
			(!metallic ? surface.ndotwi * ONE_OVER_PI : saturate(wi_s.y) * ONE_OVER_PI);

		// p_s in m_s's numerator and w_s's denominator cancel out
		const float m_s = 1.0f / max(p_s + p_d, 1e-6f);
		const float w_s = m_s * Math::Color::LuminanceFromLinearRGB(target);

		if(r.Update(w_s, Lo, wi_s, target, rng))
			r.NeedsShadowRay = 1 - TARGET_WITH_VISIBILITY;
	}

	float targetLum = Math::Color::LuminanceFromLinearRGB(r.Target);
	r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
	
#if !TARGET_WITH_VISIBILITY
	if(r.NeedsShadowRay)
	{
		if (!EvaluateVisibility(posW, r.wi, normal, linearDepth))
		{
	   		r.visible = false; 
			r.W = 0;
		}
	}
#endif	

	r.NeedsShadowRay = false;
	
	return r;
}

float PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float linearDepth)
{
	float planeDist = dot(currNormal, samplePos - currPos);
	float weight = abs(planeDist) <= MAX_PLANE_DIST_REUSE * linearDepth;

	return weight;
}

TemporalCandidate FindTemporalCandidate(uint2 DTid, float3 posW, float3 normal, float linearDepth, bool metallic, 
	float roughness, BRDF::SurfaceInteraction surface, inout RNG rng)
{
	TemporalCandidate candidate = TemporalCandidate::Init();
	const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);

	// reverse reproject current pixel
	GBUFFER_MOTION_VECTOR g_motionVector = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::MOTION_VECTOR];
	const float2 motionVec = g_motionVector[DTid.xy];
	const float2 prevUV_surface = (DTid + 0.5) / renderDim - motionVec;
	// TODO surface motion can be laggy for glossy surfaces
	float2 prevUV = prevUV_surface;

	if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
		return candidate;

	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];

	const int2 prevPixel = prevUV * renderDim;

	for(int i = 0; i < MAX_NUM_TEMPORAL_SAMPLES; i++)
	{
		const float theta = rng.Uniform() * TWO_PI;
		const float sinTheta = sin(theta);
		const float cosTheta = cos(theta);
		const int2 samplePosSS = prevPixel + (i > 0) * 8 * float2(sinTheta, cosTheta);

		if(samplePosSS.x >= renderDim.x || samplePosSS.y >= renderDim.y)
			continue;

		// plane-based heuristic
		float prevDepth = g_prevDepth[samplePosSS];
		prevDepth = Math::Transform::LinearDepthFromNDC(prevDepth, g_frame.CameraNear);

		float3 prevPos = Math::Transform::WorldPosFromScreenSpace(samplePosSS,
			renderDim,
			prevDepth,
			g_frame.TanHalfFOV,
			g_frame.AspectRatio,
			g_frame.PrevViewInv,
			g_frame.PrevProjectionJitter);

		if(!PlaneHeuristic(prevPos, normal, posW, linearDepth))
			continue;

		const float2 prevMR = g_prevMetallicRoughness[samplePosSS];

		bool prevMetallic;
		bool prevEmissive;
		GBuffer::DecodeMetallicEmissive(prevMR.x, prevMetallic, prevEmissive);

		// skip invalid reservoirs
		if(prevEmissive)
			continue;

		// normal heuristic
		const float2 prevNormalEncoded = g_prevNormal[samplePosSS];
		const float3 prevNormal = Math::Encoding::DecodeUnitVector(prevNormalEncoded);
		const float normalSimilarity = dot(prevNormal, normal);
			
		// roughness heuristic
		const float roughnessSimilarity = abs(prevMR.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE;
		candidate.valid = normalSimilarity >= MIN_NORMAL_SIMILARITY_REUSE && roughnessSimilarity && metallic == prevMetallic;

		if(candidate.valid)
		{
			candidate.posSS = (int16_t2)samplePosSS;
			candidate.posW = prevPos;
			candidate.normal = prevNormal;
			candidate.roughness = prevMR.y;

			break;
		}
	}

	return candidate;
}

void TemporalResample(TemporalCandidate candidate, float3 posW, float3 normal, float linearDepth, bool metallic,
	BRDF::SurfaceInteraction surface, inout SkyDI_Util::Reservoir r, inout RNG rng)
{
	SkyDI_Util::Reservoir prev = SkyDI_Util::PartialReadReservoir_Reuse(candidate.posSS, g_local.PrevReservoir_A_DescHeapIdx);
	const half newM = r.M + prev.M;

	{
		GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];

		float targetLumAtPrev = 0.0f;

		if(Math::Color::LuminanceFromLinearRGB(r.Le) > 1e-6)
		{
			const float3 prevBaseColor = g_prevBaseColor[candidate.posSS].rgb;
			const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
			const float3 prevWo = normalize(prevCameraPos - candidate.posW);

			BRDF::SurfaceInteraction prevSurface = BRDF::SurfaceInteraction::Init(candidate.normal, prevWo,
				metallic, candidate.roughness, prevBaseColor);

			prevSurface.SetWi(r.wi, candidate.normal);

			const float3 targetAtPrev = r.Le * BRDF::SurfaceBRDF(prevSurface);
			targetLumAtPrev = Math::Color::LuminanceFromLinearRGB(targetAtPrev);

#if TARGET_WITH_VISIBILITY == 1
			targetLumAtPrev *= Visibility(candidate.posW, r.wi, candidate.normal, linearDepth);
#endif
		}

		const float p_curr = r.M * Math::Color::LuminanceFromLinearRGB(r.Target);
		// p_temporal at sample (x) that would be mapped to current reservoir's sample (r.y),
		// followed by division by Jacobian of the mapping. Easier to compute the inverse
		// mapping and use the fact J(T(x) = y) = 1 / J(T^-1(y) = x) 
		const float J_curr_to_temporal = JacobianReconnectionShift(posW, r.wi, candidate.posW);
		const float m_curr = p_curr / max(p_curr + prev.M * targetLumAtPrev * J_curr_to_temporal, 1e-6);
		r.w_sum *= m_curr;
	}

	if(prev.IsValid())
	{
		// compute target at current pixel with temporal reservoir's sample
		surface.SetWi(prev.wi, normal);
		const float3 currTarget = prev.Le * BRDF::SurfaceBRDF(surface);
		float targetLumAtCurr = Math::Color::LuminanceFromLinearRGB(currTarget);
	
#if TARGET_WITH_VISIBILITY == 1
		targetLumAtCurr *= Visibility(posW, prev.wi, normal, linearDepth);
#endif
		
		// w_prev becomes zero; then only M needs to be updated, which is done at the end anyway
		if(targetLumAtCurr > 1e-6)
		{
			const float w_sum_prev = SkyDI_Util::PartialReadReservoir_WSum(candidate.posSS, g_local.PrevReservoir_B_DescHeapIdx);
			const float targetLumAtPrev = prev.W > 0 ? w_sum_prev / prev.W : 0;
			const float J_temporal_to_curr = JacobianReconnectionShift(candidate.posW, prev.wi, posW);
			// J_temporal_to_curr in the numerator cancels out with the same term in w_prev
			const float numerator = prev.M * targetLumAtPrev;
			const float denom = numerator / J_temporal_to_curr + r.M * targetLumAtCurr;
			// balance heuristic
			const float m_prev = numerator / max(denom, 1e-6);
			const float w_prev = m_prev * targetLumAtCurr * prev.W;

			if(r.Update(w_prev, prev.Le, prev.wi, currTarget, rng))
				r.NeedsShadowRay = true;
		}
	}

	float targetLum = Math::Color::LuminanceFromLinearRGB(r.Target);
	r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
	r.M = newM;
}

void SpatialResample(uint2 DTid, uint16_t numSamples, float radius, float3 posW, float3 normal, 
	float linearDepth, float roughness, BRDF::SurfaceInteraction surface, uint prevReservoir_A_DescHeapIdx, 
	uint prevReservoir_B_DescHeapIdx, inout SkyDI_Util::Reservoir r, inout RNG rng)
{
	static const half2 k_hammersley[8] =
	{
		half2(0.0, -0.7777777777777778),
		half2(-0.5, -0.5555555555555556),
		half2(0.5, -0.33333333333333337),
		half2(-0.75, -0.11111111111111116),
		half2(0.25, 0.11111111111111116),
		half2(-0.25, 0.33333333333333326),
		half2(0.75, 0.5555555555555556),
		half2(-0.875, 0.7777777777777777)
	};

	GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];

	// rotate sample sequence per pixel
	const float u0 = rng.Uniform();
	const uint offset = rng.UintRange(0, 8);
	const float theta = u0 * TWO_PI;
	const float sinTheta = sin(theta);
	const float cosTheta = cos(theta);

	const int2 renderDim = int2(g_frame.RenderWidth, g_frame.RenderHeight);
	PairwiseMIS pairwiseMIS = PairwiseMIS::Init(numSamples, r);

	float3 samplePosW[3];
	int16_t2 samplePosSS[3];
	float sampleRoughness[3];
	bool sampleMetallic[3];
	uint16_t k = 0;

	for (int i = 0; i < numSamples; i++)
	{
		float2 sampleUV = k_hammersley[(offset + i) & 7];
		float2 rotated;
		rotated.x = dot(sampleUV, float2(cosTheta, -sinTheta));
		rotated.y = dot(sampleUV, float2(sinTheta, cosTheta));
		rotated *= radius;
		const int2 posSS_i = round(float2(DTid) + rotated);

		if (Math::IsWithinBounds(posSS_i, renderDim))
		{
			const float depth_i = g_prevDepth[posSS_i];
			if (depth_i == 0.0)
				continue;

			const float linearDepth_i = Math::Transform::LinearDepthFromNDC(depth_i, g_frame.CameraNear);
			float3 posW_i = Math::Transform::WorldPosFromScreenSpace(posSS_i,
				renderDim,
				linearDepth_i,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.PrevViewInv,
				g_frame.PrevProjectionJitter);
			bool valid = PlaneHeuristic(posW_i, normal, posW, linearDepth);

			const float2 mr_i = g_prevMetallicRoughness[posSS_i];
			
			bool metallic_i;
			bool emissive_i;
			GBuffer::DecodeMetallicEmissive(mr_i.x, metallic_i, emissive_i);

			valid = valid && !emissive_i && (abs(mr_i.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE);

			if (!valid)
				continue;

			samplePosW[k] = posW_i;
			samplePosSS[k] = (int16_t2)posSS_i;
			sampleMetallic[k] = metallic_i;
			sampleRoughness[k] = mr_i.y;
			k++;
		}
	}

	const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
	pairwiseMIS.k = k;

	for (int i = 0; i < k; i++)
	{
		const float3 sampleNormal = Math::Encoding::DecodeUnitVector(g_prevNormal[samplePosSS[i]]);
		const float3 sampleBaseColor = g_prevBaseColor[samplePosSS[i]].rgb;

		const float3 wo_i = normalize(prevCameraPos - samplePosW[i]);
		BRDF::SurfaceInteraction surface_i = BRDF::SurfaceInteraction::Init(sampleNormal, wo_i,
			sampleMetallic[i], sampleRoughness[i], sampleBaseColor);

		SkyDI_Util::Reservoir neighbor = SkyDI_Util::PartialReadReservoir_Reuse(samplePosSS[i], g_local.PrevReservoir_A_DescHeapIdx);
		const float neighborWSum = SkyDI_Util::PartialReadReservoir_WSum(samplePosSS[i], prevReservoir_B_DescHeapIdx);

		pairwiseMIS.Stream(r, posW, normal, linearDepth, surface, neighbor, samplePosW[i], 
			sampleNormal, neighborWSum, surface_i, rng);
	}

	pairwiseMIS.End(r, rng);
	r = pairwiseMIS.r_s;
}

SkyDI_Util::Reservoir EstimateDirectLighting(uint2 DTid, float3 posW, float3 normal, float linearDepth, 
	bool metallic, float roughness, float3 baseColor, BRDF::SurfaceInteraction surface, inout RNG rng)
{
	SkyDI_Util::Reservoir r = RIS_InitialCandidates(DTid, posW, normal, linearDepth, metallic, roughness,
		surface, rng);

	// skip resampling for mirror-like metals & dark-colored glossy dielectrics
	const bool resample = (roughness > g_local.MinRoughnessResample || (!metallic && Math::Color::LuminanceFromLinearRGB(baseColor) > 1e-2));
	
	if (IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::TEMPORAL_RESAMPLE) && resample) 
	{
		TemporalCandidate candidate = FindTemporalCandidate(DTid, posW, normal, linearDepth, metallic, roughness, 
			surface, rng);

		if(candidate.valid)
			TemporalResample(candidate, posW, normal, linearDepth, metallic, surface, r, rng);
	}

	float m_max = 2 + smoothstep(0, 0.6, roughness * roughness) * g_local.M_max;
	
	SkyDI_Util::WriteReservoir(DTid, r, g_local.CurrReservoir_A_DescHeapIdx,
			g_local.CurrReservoir_B_DescHeapIdx, half(m_max));

	// spatial resampling
	if(IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::SPATIAL_RESAMPLE) && resample)
	{
		SpatialResample(DTid, NUM_SPATIAL_SAMPLES, SPATIAL_SEARCH_RADIUS, posW, normal, linearDepth, roughness, surface, 
			g_local.PrevReservoir_A_DescHeapIdx, g_local.PrevReservoir_B_DescHeapIdx, r, rng);
	}

#if TARGET_WITH_VISIBILITY == 0
	if (r.NeedsShadowRay)
		r.Visible = Visibility(g_bvh, posW, target.wi, target.rayT, normal, linearDepth, target.lightID);
#endif

	return r;
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[numthreads(SKY_DI_TEMPORAL_GROUP_DIM_X, SKY_DI_TEMPORAL_GROUP_DIM_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex, uint3 GTid : SV_GroupThreadID)
{
#if THREAD_GROUP_SWIZZLING
	// swizzle thread groups for better L2-cache behavior
	// Ref: https://developer.nvidia.com/blog/optimizing-compute-shaders-for-l2-locality-using-thread-group-id-swizzling/
	uint2 swizzledDTid = Common::SwizzleThreadGroup(DTid, Gid, GTid, uint16_t2(SKY_DI_TEMPORAL_GROUP_DIM_X, SKY_DI_TEMPORAL_GROUP_DIM_Y),
		g_local.DispatchDimX, SKY_DI_TEMPORAL_TILE_WIDTH, SKY_DI_TEMPORAL_LOG2_TILE_WIDTH, g_local.NumGroupsInTile);
#else
	const uint2 swizzledDTid = DTid.xy;
#endif

	if (swizzledDTid.x >= g_frame.RenderWidth || swizzledDTid.y >= g_frame.RenderHeight)
		return;

	GBUFFER_DEPTH g_depth = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
	const float depth = g_depth[swizzledDTid];
	
	if(depth == 0)
		return;
		
	// reconstruct position from depth buffer
	const float linearDepth = Math::Transform::LinearDepthFromNDC(depth, g_frame.CameraNear);
	const uint2 renderDim = uint2(g_frame.RenderWidth, g_frame.RenderHeight);
	const float3 posW = Math::Transform::WorldPosFromScreenSpace(swizzledDTid,
		renderDim,
		linearDepth,
		g_frame.TanHalfFOV,
		g_frame.AspectRatio,
		g_frame.CurrViewInv,
		g_frame.CurrProjectionJitter);
	
	// shading normal
	GBUFFER_NORMAL g_normal = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
	const float3 normal = Math::Encoding::DecodeUnitVector(g_normal[swizzledDTid]);
		
	// roughness and metallic mask
	GBUFFER_METALLIC_ROUGHNESS g_metallicRoughness = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::METALLIC_ROUGHNESS];
	const float2 mr = g_metallicRoughness[swizzledDTid];

	bool metallic;
	bool isEmissive;
	GBuffer::DecodeMetallicEmissive(mr.x, metallic, isEmissive);

	if (isEmissive)
		return;

	GBUFFER_BASE_COLOR g_baseColor = ResourceDescriptorHeap[g_frame.CurrGBufferDescHeapOffset +
		GBUFFER_OFFSET::BASE_COLOR];
	const float3 baseColor = g_baseColor[swizzledDTid].rgb;

	const float3 wo = normalize(g_frame.CameraPos - posW);
	BRDF::SurfaceInteraction surface = BRDF::SurfaceInteraction::Init(normal, wo, metallic, mr.y, baseColor);

	RNG rng = RNG::Init(swizzledDTid, g_frame.FrameNum);

	// generate candidates and spatiotemporal resampling
	SkyDI_Util::Reservoir r = EstimateDirectLighting(swizzledDTid, posW, normal, linearDepth, metallic, mr.y, baseColor, surface, rng);

	if(IS_CB_FLAG_SET(CB_SKY_DI_FLAGS::DENOISE))
	{
		// split into diffuse & specular, so they can be denoised seperately
		surface.SetWi(r.wi, normal);

		// demodulate base color
		float3 f_d = (1.0f - surface.F) * (1.0f - metallic) * surface.ndotwi * ONE_OVER_PI;
		// demodulate Fresnel for metallic surfaces to preserve texture detail
		float3 f_s = !metallic ? 
			BRDF::SpecularBRDFGGXSmith(surface, true) : 
			(surface.DeltaNDF ? abs(1 - surface.ndotwh) < 5e-4 : BRDF::GGX(surface.ndotwh, surface.alphaSq)) *
			BRDF::SmithHeightCorrelatedG2ForGGX(surface.alphaSq, surface.ndotwi, surface.ndotwo) * 
			surface.ndotwi;

		float3 Li_d = r.Le * f_d * r.Visible * r.W;
		float3 Li_s = r.Le * f_s * r.Visible * r.W;
		float tmp = 1.0f - surface.whdotwo;
		tmp = tmp * tmp * tmp * tmp * tmp;
		uint tmpU = asuint(tmp);
		half2 encoded = half2(asfloat16(uint16_t(tmpU & 0xffff)), asfloat16(uint16_t(tmpU >> 16)));

		RWTexture2D<float4> g_colorA = ResourceDescriptorHeap[g_local.ColorAUavDescHeapIdx];
		RWTexture2D<half4> g_colorB = ResourceDescriptorHeap[g_local.ColorBUavDescHeapIdx];

		g_colorA[swizzledDTid] = float4(Li_s, Li_d.r);
		g_colorB[swizzledDTid] = half4(Li_d.gb, encoded);
	}
	else
	{
		float3 Li = r.Target * r.Visible * r.W;
		RWTexture2D<float4> g_final = ResourceDescriptorHeap[g_local.FinalDescHeapIdx];

		if(g_frame.Accumulate && g_frame.CameraStatic)
		{
			float3 prev = g_final[swizzledDTid].rgb;
			g_final[swizzledDTid].rgb = prev + Li;
		}
		else
			g_final[swizzledDTid].rgb = Li;
	}
}