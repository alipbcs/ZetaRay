#ifndef RESTIR_DI_RESAMPLING_H
#define RESTIR_DI_RESAMPLING_H

#include "DirectLighting_Common.h"
#include "ReSTIR_DI_Reservoir.hlsli"
#include "ReSTIR_DI.hlsli"
#include "../../Common/FrameConstants.h"
#include "../../Common/Sampling.hlsli"
#include "../../Common/BRDF.hlsli"
#include "../../Common/RT.hlsli"
#include "../../Common/GBuffers.hlsli"
#include "../../Common/StaticTextureSamplers.hlsli"

namespace RDI_Util
{
	struct EmissiveData
	{
		static EmissiveData Init(Reservoir r, StructuredBuffer<RT::EmissiveTriangle> g_emissives)
		{
			EmissiveData ret;

			if(r.IsValid())
			{
				RT::EmissiveTriangle tri = g_emissives[r.LightIdx];
				ret.ID = tri.ID;

				const float3 vtx1 = tri.V1();
				const float3 vtx2 = tri.V2();
				ret.lightPos = (1.0f - r.Bary.x - r.Bary.y) * tri.Vtx0 + r.Bary.x * vtx1 + r.Bary.y * vtx2;

				ret.lightNormal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
				ret.lightNormal = all(ret.lightNormal == 0) ? ret.lightNormal : normalize(ret.lightNormal);
				//ret.lightNormal = tri.IsDoubleSided() && dot(-ret.wi, ret.lightNormal) < 0 ? ret.lightNormal * -1.0f : ret.lightNormal;
				ret.doubleSided = tri.IsDoubleSided();
			}

			return ret;
		}

		void SetSurfacePos(float3 posW)
		{
			this.t = length(this.lightPos - posW);
			this.wi = (this.lightPos - posW) / this.t;
			this.lightNormal = this.doubleSided && dot(-this.wi, this.lightNormal) < 0 ? this.lightNormal * -1.0f : this.lightNormal;
		}

		float dWdA()
		{
			float cosThetaPrime = saturate(dot(this.lightNormal, -this.wi));
			// [hack] due to using 16-bit floats for triangle vertices, light normal can become zero for small or thin triangles
			cosThetaPrime = all(this.lightNormal == 0) ? 1 : cosThetaPrime;
			float dWdA = cosThetaPrime / (this.t * this.t);

			return dWdA;
		}

		float3 wi;
		float t;
		uint ID;
		float3 lightPos;
		float3 lightNormal;
		bool doubleSided;
	};

	struct Target
	{
		static Target Init()
		{
			Target ret;

			ret.p_hat = 0.0.xxx;
			ret.lightID = -1;
			ret.dwdA = 0;
			ret.needsShadowRay = false;
			ret.visible = true;
		
			return ret;
		}

		float3 p_hat;
		float rayT;
		uint lightID;
		float3 wi;
		float3 lightNormal;
		float3 lightPos;
		float dwdA;
		// store visibility seperately as biased version doesn't include it in target
		bool needsShadowRay;
		bool visible;
	};

	struct EmissiveTriSample
	{
		float3 pos;
		float3 normal;
		float2 bary;
		float pdf;
	};

	struct BrdfHitInfo
	{
		uint emissiveTriIdx;
		float2 bary;
		float3 lightPos;
		float t;
	};

	struct TemporalCandidate
	{
		static TemporalCandidate Init()
		{
			TemporalCandidate ret;
			ret.valid = false;
			ret.lightIdx = -1;
		
			return ret;
		}

		float3 posW;
		uint lightIdx;
		float3 normal;
		float roughness;
		int16_t2 posSS;
		bool valid;
		bool metallic;
	};

	bool Visibility(RaytracingAccelerationStructure g_sceneBVH, float3 pos, float3 wi, float rayT,
		float3 normal, float linearDepth, uint triID)
	{
		// HACK use a larger offset to avoid intersection with decals
		float tMin;
		const float3 adjustedOrigin = RT::OffsetRay(pos, normal, linearDepth, tMin, 6e-3f);

		RayQuery<RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
			RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			RAY_FLAG_FORCE_OPAQUE> rayQuery;
	
		RayDesc ray;
		ray.Origin = adjustedOrigin;
		ray.TMin = tMin;
		ray.TMax = rayT;
		ray.Direction = wi;

		// Initialize
		rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::NON_EMISSIVE, ray);

		// Traversal
		rayQuery.Proceed();

		// triangle intersection only when hit_t < t_max
		if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), rayQuery.CommittedPrimitiveIndex());
			uint hash = RNG::Pcg3d(key).x;

			return triID == hash;
		}

		return true;
	}

	bool VisibilityApproximate(RaytracingAccelerationStructure g_sceneBVH, float3 pos, float3 wi, float rayT,
		float3 normal, float linearDepth, uint triID)
	{
		// HACK use a larger offset to avoid intersection with decals
		float q = wi.y < 0 ? 1e-4f : 1e-2f;
		float tMin;
		const float3 adjustedOrigin = RT::OffsetRay(pos, normal, linearDepth, tMin, q);

		// To test for occlusion against some light source at distance t_l we need to check if 
		// the ray hits any geometry for which t_hit < t_l. According to dxr specs, for any committed triangle 
		// hit, t_hit < t_max. So we set t_max = t_l and trace a ray. As any such hit indicates occlusion,
		// the RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH flag can be specified for improved 
		// perfromance. Now due to floating-point precision issues, it's possible that the first hit 
		// could be the light source itself -- t_hit ~= t_ray. In this scenario, occlusion is inconclusive 
		// as there may or may not be other occluders along the ray with t < t_hit. As an approximation, 
		// the following decreases t_l by a small amount to avoid the situation described above.
		RayQuery<RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
			RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
			RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
			RAY_FLAG_FORCE_OPAQUE> rayQuery;

		RayDesc ray;
		ray.Origin = adjustedOrigin;
		ray.TMin = tMin;
		ray.TMax = 0.995f * rayT;
		ray.Direction = wi;

		// Initialize
		rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::NON_EMISSIVE, ray);

		// Traversal
		rayQuery.Proceed();

		// triangle intersection only when hit_t < t_max
		if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			uint3 key = uint3(rayQuery.CommittedGeometryIndex(), rayQuery.CommittedInstanceID(), rayQuery.CommittedPrimitiveIndex());
			uint hash = RNG::Pcg3d(key).x;

			return triID == hash;
		}

		return true;
	}

	// Ref: Bitterli, Benedikt, "Correlations and Reuse for Fast and Accurate Physically Based Light Transport" (2022). Ph.D Dissertation.
	// https://digitalcommons.dartmouth.edu/dissertations/77
	struct PairwiseMIS
	{	
		static PairwiseMIS Init(uint16_t numStrategies, Reservoir r_c)
		{
			PairwiseMIS ret;

			ret.r_s = Reservoir::Init();
			ret.m_c = 1.0f;
			ret.M_s = r_c.M;
			ret.k = numStrategies;
			ret.target_s = Target::Init();

			return ret;
		}

		float Compute_m_i(Reservoir r_c, Reservoir r_i, float targetLum, float w_sum_i)
		{
			// TODO following seems to be the correct term to use, but for some reason gives terrible results
#if 0
			const float p_i_y_i = r_i.W > 0 ? w_sum_i / r_i.W : 0;
#else
			const float p_i_y_i = r_i.W > 0 ? (r_i.M * w_sum_i) / r_i.W : 0;
#endif

			const float p_c_y_i = targetLum * r_c.M;
			float m_i = r_i.M * p_i_y_i;
			float denom = m_i + (r_c.M / this.k) * p_c_y_i;
			m_i = denom > 0 ? m_i / denom : 0;

			return m_i;
		}

		void Update_m_c(Reservoir r_c, Reservoir r_i, Target target_c, float3 brdfCosTheta_i, float3 wi_i, float t_i)
		{
			if(!r_c.IsValid())
			{
				this.m_c += 1;
				return;
			}

			const float cosThetaPrime = saturate(dot(target_c.lightNormal, -wi_i));
			const float dwdA = cosThetaPrime / max(t_i * t_i, 1e-6);
			const float p_i_y_c = Math::Color::LuminanceFromLinearRGB(r_c.Le * brdfCosTheta_i * dwdA);
			const float p_c_y_c = Math::Color::LuminanceFromLinearRGB(target_c.p_hat);

			const float numerator = r_i.M * p_i_y_c;
			const bool denomGt0 = (p_c_y_c + numerator) > 0;	// both are positive
			this.m_c += denomGt0 ? 1 - numerator / (numerator + (r_c.M / this.k) * p_c_y_c) : 1;
		}

		void Stream(Reservoir r_c, float3 posW_c, float3 normal_c, float linearDepth_c, 
			BRDF::SurfaceInteraction surface_c, Reservoir r_i, float3 posW_i, float3 normal_i, float w_sum_i, 
			BRDF::SurfaceInteraction surface_i, StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
			RaytracingAccelerationStructure g_bvh, Target target_c, inout RNG rng)
		{
			float3 currTarget;
			float m_i;
			EmissiveData emissive_i;
			float dwdA;

			// m_i
			if(r_i.IsValid())
			{
				emissive_i = EmissiveData::Init(r_i, g_emissives);
				emissive_i.SetSurfacePos(posW_c);
				dwdA = emissive_i.dWdA();

				surface_c.SetWi(emissive_i.wi, normal_c);
				const float3 brdfCosTheta_c = BRDF::SurfaceBRDF(surface_c);
				currTarget = r_i.Le * brdfCosTheta_c * dwdA;

#if TARGET_WITH_VISIBILITY == 1
				if(Math::Color::LuminanceFromLinearRGB(currTarget) > 1e-5)
					currTarget *= VisibilityApproximate(g_bvh, posW_c, emissive_i.wi, emissive_i.t, normal_c, linearDepth_c, emissive_i.ID);
#endif

				const float targetLum = Math::Color::LuminanceFromLinearRGB(currTarget);
				m_i = Compute_m_i(r_c, r_i, targetLum, w_sum_i);
			}

			// m_c
			float3 brdfCosTheta_i = 0.0.xxx;
			float3 wi_i = 0.0.xxx;
			float t_i = 0;

			if(r_c.IsValid())
			{
				t_i = length(target_c.lightPos - posW_i);
				wi_i = (target_c.lightPos - posW_i) / t_i;
				surface_i.SetWi(wi_i, normal_i);
				brdfCosTheta_i = BRDF::SurfaceBRDF(surface_i);

#if TARGET_WITH_VISIBILITY == 1
				if(Math::Color::LuminanceFromLinearRGB(brdfCosTheta_i) > 1e-5)
					brdfCosTheta_i *= VisibilityApproximate(g_bvh, posW_i, wi_i, t_i, normal_i, linearDepth_c, target_c.lightID);
#endif
			}

			Update_m_c(r_c, r_i, target_c, brdfCosTheta_i, wi_i, t_i);

			if(r_i.IsValid())
			{
				const float w_i = m_i * Math::Color::LuminanceFromLinearRGB(currTarget) * r_i.W;

				if (this.r_s.Update(w_i, r_i.Le, r_i.LightIdx, r_i.Bary, rng))
				{
					this.target_s.p_hat = currTarget;
					this.target_s.rayT = emissive_i.t;
					this.target_s.lightID = emissive_i.ID;
					this.target_s.wi = emissive_i.wi;
					this.target_s.dwdA = dwdA;
					this.target_s.needsShadowRay = 1 - TARGET_WITH_VISIBILITY;
				}
			}

			this.M_s += r_i.M;
		}

		void End(Reservoir r_c, inout Target target_c, inout RNG rng)
		{
			const float w_c = Math::Color::LuminanceFromLinearRGB(target_c.p_hat) * r_c.W * this.m_c;

			if(!this.r_s.Update(w_c, r_c.Le, r_c.LightIdx, r_c.Bary, rng))
			{
				target_c = this.target_s;
				target_c.needsShadowRay = target_c.needsShadowRay || target_s.needsShadowRay;
			}

			this.r_s.M = this.M_s;
			const float targetLum = Math::Color::LuminanceFromLinearRGB(target_c.p_hat);
			this.r_s.W = targetLum > 0 ? this.r_s.w_sum / (targetLum * (1 + this.k)) : 0;
			// TODO investigate
			this.r_s.W = isnan(this.r_s.W) ? 0 : this.r_s.W;
		}

		Reservoir r_s;
		float m_c;
		half M_s;
		uint16_t k;
		Target target_s;
	};

	uint SampleAliasTable(StructuredBuffer<EmissiveTriangleSample> g_aliasTable, uint numEmissiveTriangles, inout RNG rng, out float pdf)
	{
		uint u0 = rng.UintRange(0, numEmissiveTriangles);
		EmissiveTriangleSample s = g_aliasTable[u0];

		float u1 = rng.Uniform();
		if (u1 <= s.P_Curr)
		{
			pdf = s.CachedP_Orig;
			return u0;
		}

		pdf = s.CachedP_Alias;
		return s.Alias;
	}

	uint UnformSampleSampleSet(uint sampleSetIdx, StructuredBuffer<LightSample> g_sampleSets, uint sampleSetSize, inout RNG rng, 
		out RT::EmissiveTriangle tri, out float pdf)
	{
		uint u = rng.UintRange(0, sampleSetSize);

		LightSample s = g_sampleSets[sampleSetIdx * sampleSetSize + u];
		tri = s.Tri;
		pdf = s.Pdf;

		return s.Index;
	}

	EmissiveTriSample SampleEmissiveTriangleSurface(float3 posW, RT::EmissiveTriangle tri, inout RNG rng)
	{
		EmissiveTriSample ret;

		float2 u = rng.Uniform2D();
		ret.bary = Sampling::UniformSampleTriangle(u);

		const float3 vtx1 = tri.V1();
		const float3 vtx2 = tri.V2();
		ret.pos = (1.0f - ret.bary.x - ret.bary.y) * tri.Vtx0 + ret.bary.x * vtx1 + ret.bary.y * vtx2;
		ret.normal = cross(vtx1 - tri.Vtx0, vtx2 - tri.Vtx0);
		float twoArea = length(ret.normal);
		twoArea = max(twoArea, 1e-6);
		ret.pdf = all(ret.normal == 0) ? 1.0f : 1.0f / (0.5f * twoArea);

		ret.normal = all(ret.normal == 0) ? ret.normal : ret.normal / twoArea;
		ret.normal = tri.IsDoubleSided() && dot(posW - ret.pos, ret.normal) < 0 ? ret.normal * -1.0f : ret.normal;

		return ret;
	}

	bool FindClosestHit(float3 pos, float3 normal, float3 wi, float linearDepth, 
		RaytracingAccelerationStructure g_sceneBVH, StructuredBuffer<RT::MeshInstance> g_frameMeshData,
		out BrdfHitInfo hitInfo)
	{
		float tMin;
		const float3 adjustedOrigin = RT::OffsetRay(pos, normal, linearDepth, tMin);

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> rayQuery;

		RayDesc ray;
		ray.Origin = adjustedOrigin;
		ray.TMin = tMin;
		ray.TMax = FLT_MAX;
		ray.Direction = wi;

		// Initialize
		rayQuery.TraceRayInline(g_sceneBVH, RAY_FLAG_NONE, RT_AS_SUBGROUP::ALL, ray);

		// Traversal
		rayQuery.Proceed();

		if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			const RT::MeshInstance meshData = g_frameMeshData[rayQuery.CommittedGeometryIndex() + rayQuery.CommittedInstanceID()];

			if (meshData.BaseEmissiveTriOffset == -1)
				return false;

			hitInfo.emissiveTriIdx = meshData.BaseEmissiveTriOffset + rayQuery.CommittedPrimitiveIndex();
			hitInfo.bary = rayQuery.CommittedTriangleBarycentrics();
			hitInfo.lightPos = rayQuery.WorldRayOrigin() + rayQuery.WorldRayDirection() * rayQuery.CommittedRayT();
			hitInfo.t = rayQuery.CommittedRayT();

			return true;
		}

		return false;
	}

	// assumes area light is diffuse
	float3 EmissiveTriangleLi(RT::EmissiveTriangle tri, float2 bary, uint emissiveMapsDescHeapOffset)
	{
		const float3 emissiveFactor = Math::Color::UnpackRGB(tri.EmissiveFactor_Signs);
		const float emissiveStrength = tri.GetEmissiveStrength();
		float3 L_e = emissiveFactor * emissiveStrength;

		if (Math::Color::LuminanceFromLinearRGB(L_e) <= 1e-5)
			return 0.0.xxx;

		uint16_t emissiveTex = tri.GetEmissiveTex();
		if (emissiveTex != -1)
		{
			const uint offset = NonUniformResourceIndex(emissiveMapsDescHeapOffset + emissiveTex);
			EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[offset];

			float2 texUV = (1.0f - bary.x - bary.y) * tri.UV0 + bary.x * tri.UV1 + bary.y * tri.UV2;
			L_e *= g_emissiveMap.SampleLevel(g_samLinearWrap, texUV, 0).rgb;
		}

		return L_e;
	}

	float PlaneHeuristic(float3 samplePos, float3 currNormal, float3 currPos, float linearDepth)
	{
		float planeDist = dot(currNormal, samplePos - currPos);
		float weight = abs(planeDist) <= MAX_PLANE_DIST_REUSE * linearDepth;

		return weight;
	}

	TemporalCandidate FindTemporalCandidates(uint2 DTid, float3 posW, float3 normal, float linearDepth, float roughness, 
		float2 prevUV, ConstantBuffer<cbFrameConstants> g_frame, inout RNG rng)
	{
		TemporalCandidate candidate = RDI_Util::TemporalCandidate::Init();

		if (any(prevUV < 0.0f.xx) || any(prevUV > 1.0f.xx))
			return candidate;

		GBUFFER_DEPTH g_prevDepth = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::DEPTH];
		GBUFFER_NORMAL g_prevNormal = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset + GBUFFER_OFFSET::NORMAL];
		GBUFFER_METALLIC_ROUGHNESS g_prevMetallicRoughness = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
			GBUFFER_OFFSET::METALLIC_ROUGHNESS];

		const float2 renderDim = float2(g_frame.RenderWidth, g_frame.RenderHeight);
		const int2 prevPixel = prevUV * renderDim;

		for(int i = 0; i < NUM_TEMPORAL_SEARCH_ITER; i++)
		{
			const float theta = rng.Uniform() * TWO_PI;
			const float sinTheta = sin(theta);
			const float cosTheta = cos(theta);
			const int2 samplePosSS = prevPixel + (i > 0) * TEMPORAL_SEARCH_RADIUS * float2(sinTheta, cosTheta);

			if(samplePosSS.x >= renderDim.x || samplePosSS.y >= renderDim.y)
				continue;

			// plane-based heuristic
			float prevDepth = g_prevDepth[samplePosSS];
			prevDepth = Math::Transform::LinearDepthFromNDC(prevDepth, g_frame.CameraNear);

			const float3 prevPos = Math::Transform::WorldPosFromScreenSpace(samplePosSS,
				renderDim,
				prevDepth,
				g_frame.TanHalfFOV,
				g_frame.AspectRatio,
				g_frame.PrevViewInv,
				g_frame.PrevProjectionJitter);

			if(!RDI_Util::PlaneHeuristic(prevPos, normal, posW, linearDepth))
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

			candidate.valid = normalSimilarity >= MIN_NORMAL_SIMILARITY_REUSE && roughnessSimilarity;

			if(candidate.valid)
			{
				candidate.posSS = (int16_t2)samplePosSS;
				candidate.posW = prevPos;
				candidate.normal = prevNormal;
				candidate.metallic = prevMetallic;
				candidate.roughness = prevMR.y;

				break;
			}
		}

		return candidate;
	}

	float TargetLumAtTemporalPixel(float3 le, float3 lightPos, float3 lightNormal, uint lightID, uint2 posSS, float3 prevPosW, 
		float3 prevNormal, bool prevMetallic, float prevRoughness, float prevLinearDepth, ConstantBuffer<cbFrameConstants> g_frame,
		RaytracingAccelerationStructure g_bvh, out BRDF::SurfaceInteraction prevSurface)
	{
		GBUFFER_BASE_COLOR g_prevBaseColor = ResourceDescriptorHeap[g_frame.PrevGBufferDescHeapOffset +
			GBUFFER_OFFSET::BASE_COLOR];

		const float3 prevBaseColor = g_prevBaseColor[posSS].rgb;
		const float3 prevCameraPos = float3(g_frame.PrevViewInv._m03, g_frame.PrevViewInv._m13, g_frame.PrevViewInv._m23);
		const float3 prevWo = normalize(prevCameraPos - prevPosW);

		prevSurface = BRDF::SurfaceInteraction::Init(prevNormal, prevWo, prevMetallic, prevRoughness, prevBaseColor);

		const float t = length(lightPos - prevPosW);
		const float3 wi = (lightPos - prevPosW) / t;
		prevSurface.SetWi(wi, prevNormal);

		float cosThetaPrime = saturate(dot(lightNormal, -wi));
		cosThetaPrime = all(lightNormal == 0) ? 1 : cosThetaPrime;
		const float dwdA = cosThetaPrime / max(t * t, 1e-6f);

		const float3 targetAtPrev = le * BRDF::SurfaceBRDF(prevSurface) * dwdA;
		float targetLumAtPrev = Math::Color::LuminanceFromLinearRGB(targetAtPrev);

		// should use previous frame's bvh
#if TARGET_WITH_VISIBILITY == 1
		targetLumAtPrev *= VisibilityApproximate(g_bvh, prevPosW, wi, t, prevNormal, prevLinearDepth, lightID);
#endif

		return targetLumAtPrev;
	}

	float3 TargetAtCurrentPixel(float3 le, float3 posW, float3 normal, float linearDepth, BRDF::SurfaceInteraction surface, 
		RaytracingAccelerationStructure g_bvh, inout EmissiveData prevEmissive, out float dwdA)
	{
		prevEmissive.SetSurfacePos(posW);
		dwdA = prevEmissive.dWdA();

		surface.SetWi(prevEmissive.wi, normal);
		float3 target = le * BRDF::SurfaceBRDF(surface) * dwdA;
	
#if TARGET_WITH_VISIBILITY == 1
		target *= VisibilityApproximate(g_bvh, posW, prevEmissive.wi, prevEmissive.t, normal, linearDepth, prevEmissive.ID);
#endif

		return target;
	}

	void TemporalResample1(float3 posW, float3 normal, float roughness, float linearDepth, BRDF::SurfaceInteraction surface, 
		uint PrevReservoir_A_DescHeapIdx, uint PrevReservoir_B_DescHeapIdx, TemporalCandidate candidate,
		ConstantBuffer<cbFrameConstants> g_frame, StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
		RaytracingAccelerationStructure g_bvh, inout Reservoir r, inout RNG rng, inout Target target)
	{
		const half prevM = PartialReadReservoir_M(candidate.posSS, PrevReservoir_A_DescHeapIdx);
		const half newM = r.M + prevM;

		{
			float targetLumAtPrev = 0.0f;

			if(Math::Color::LuminanceFromLinearRGB(r.Le) > 1e-6)
			{
				BRDF::SurfaceInteraction prevSurface;
				targetLumAtPrev = TargetLumAtTemporalPixel(r.Le, target.lightPos, target.lightNormal, 
					target.lightID, candidate.posSS, candidate.posW, 
					candidate.normal, candidate.metallic, candidate.roughness, 
					linearDepth, g_frame, g_bvh, prevSurface);
			}

			const float p_curr = r.M * Math::Color::LuminanceFromLinearRGB(target.p_hat);
			const float m_curr = p_curr / max(p_curr + prevM * targetLumAtPrev, 1e-6);
			r.w_sum *= m_curr;
		}

		if(candidate.lightIdx != -1)
		{
			Reservoir prev = PartialReadReservoir_ReuseRest(candidate.posSS,
				PrevReservoir_A_DescHeapIdx, 
				candidate.lightIdx);

			// compute target at current pixel with previous reservoir's sample
			EmissiveData prevEmissive = EmissiveData::Init(prev, g_emissives);
			float dwdA;
			const float3 currTarget = TargetAtCurrentPixel(prev.Le, posW, normal, linearDepth, surface, g_bvh, prevEmissive, dwdA);
			const float targetLumAtCurr = Math::Color::LuminanceFromLinearRGB(currTarget);

			// w_prev becomes zero; then only M needs to be updated, which is done at the end anyway
			if(targetLumAtCurr > 1e-6)
			{
				const float w_sum_prev = PartialReadReservoir_WSum(candidate.posSS, PrevReservoir_B_DescHeapIdx);
				const float targetLumAtPrev = prev.W > 0 ? w_sum_prev / prev.W : 0;
				// balance heuristic
				const float p_prev = prev.M * targetLumAtPrev;
				const float m_prev = p_prev / max(p_prev + r.M * targetLumAtCurr, 1e-6);
				const float w_prev = m_prev * targetLumAtCurr * prev.W;

				if(r.Update(w_prev, prev.Le, prev.LightIdx, prev.Bary, rng))
				{
					target.p_hat = currTarget;
					target.rayT = prevEmissive.t;
					target.lightID = prevEmissive.ID;
					target.wi = prevEmissive.wi;
					target.lightNormal = prevEmissive.lightNormal;
					target.lightPos = prevEmissive.lightPos;
					target.dwdA = dwdA;
					target.needsShadowRay = 1 - TARGET_WITH_VISIBILITY;
				}
			}
		}

		float targetLum = Math::Color::LuminanceFromLinearRGB(target.p_hat);
		r.W = targetLum > 0.0 ? r.w_sum / targetLum : 0.0;
		r.M = newM;
	}

	void SpatialResample(uint2 DTid, uint16_t numSamples, float radius, float3 posW, float3 normal, 
		float linearDepth, float roughness, BRDF::SurfaceInteraction surface, uint prevReservoir_A_DescHeapIdx, 
		uint prevReservoir_B_DescHeapIdx, ConstantBuffer<cbFrameConstants> g_frame, StructuredBuffer<RT::EmissiveTriangle> g_emissives, 
		RaytracingAccelerationStructure g_bvh, inout Reservoir r, inout Target target, inout RNG rng)
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

		float3 samplePosW[4];
		int16_t2 samplePosSS[4];
		uint sampleLightIdx[4];
		float sampleRoughness[4];
		bool sampleMetallic[4];
		uint16_t k = 0;

		[loop]
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

				// TODO ignoring this causes strange frame time spikes when sky is visible, due to specular brdf evaluation
				valid = valid && !emissive_i && (abs(mr_i.y - roughness) < MAX_ROUGHNESS_DIFF_REUSE);

				if (!valid)
					continue;

				const uint lightIdx_i = PartialReadReservoir_ReuseLightIdx(posSS_i, prevReservoir_B_DescHeapIdx);

				samplePosW[k] = posW_i;
				samplePosSS[k] = (int16_t2)posSS_i;
				sampleLightIdx[k] = lightIdx_i;
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
				
			// TODO is capping M needed?
			Reservoir neighbor = PartialReadReservoir_ReuseRest(samplePosSS[i],
				prevReservoir_A_DescHeapIdx,
				sampleLightIdx[i]);

			const float neighborWSum = PartialReadReservoir_WSum(samplePosSS[i], prevReservoir_B_DescHeapIdx);

			pairwiseMIS.Stream(r, posW, normal, linearDepth, surface, neighbor, samplePosW[i], 
				sampleNormal, neighborWSum, surface_i, g_emissives, g_bvh, target, rng);
		}

		pairwiseMIS.End(r, target, rng);
		r = pairwiseMIS.r_s;
	}
}
#endif