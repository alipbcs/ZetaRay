// Refs:
//
// 1. M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2016.
// 2. E. Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs," Journal of Computer Graphics Techniques, 2014.
// 3. B. Walter, S.R. Marschner1, H. Li, K.E. Torrance, "Microfacet Models for Refraction through Rough Surfaces," in EGSR'07, 2007.
// 4. J. Boksansky, "Crash Course in BRDF Implementation," 2021. [Online]. Available: https://boksajak.github.io/blog/BRDF.
//
// In microfacet models, a macrosurface BRDF is designed to match the aggregate behavior of 
// visible micorsurfaces that constitute that macrosurface. Microsurfaces can be statistically 
// described by a normal distribution function (NDF), a shadowing-masking function (G) and a 
// microsurface BRDF that describes how each microsurface scatters light. Commonly, a perfect 
// specular (mirror) BRDF is assumed for microsurfaces. Furthermore, product of NDF and G gives 
// the visible area of each microsurface.
//
// Therefore, macrosurface BRDF is the integral of contributions of scatterd lights from 
// individual visible microsurfaces where each microsurface scatters light according to its 
// microsurface BRDF (assumed to be perfect mirror).
//
// Solving above gives the macrosruface BRDF as:
//		f(p, wi, wo) = (F(wo, h) * NDF(h) * G(wi, wo, h)) / (4 * |n.wi| * |n.wo|)
//
// where h is the half vector = normalize(wi + wo).
//
// Finally, a choice for NDF and G functions needs to be made (G depends on the chosen NDF).
// Here GGX normal distribution with the height-correlated Smith shadowing-masking function
// is used.

#ifndef BRDF_H
#define BRDF_H

#include "Math.hlsli"
#include "Sampling.hlsli"

#define USE_VNDF_SPHERICAL_CAPS 1

// About 1 degrees. To check against (almost) perfect reflection.
#define MIN_N_DOT_H_PERFECT_SPECULAR 0.9998157121216442

namespace BRDF
{
	//--------------------------------------------------------------------------------------
	// Fresnel
	//--------------------------------------------------------------------------------------
	// Note: Schlick's original approximation
	//		R(theta) = R0 + (1 - R0)(1 - cos(theta))^5
	// gives reflectivity for a given wavelength (a scalar) with the following assumptions:
	//
	// 1. Surface is dielectric.
	// 2. eta1 (incoming material) < eta2 (outgoing material) (from less dense to denser).
	//
	// Also when eta1 > eta2, total internal reflection needs to be accounted for, which
	// happens at incident angle arcsin(eta2 / eta1)
	//--------------------------------------------------------------------------------------
	float3 FresnelSchlick(float3 F0, float whdotwo)
	{
		float tmp = 1.0f - whdotwo;
		float tmpSq = tmp * tmp;
		return F0 + (1.0f - F0) * tmpSq * tmpSq * tmp;
	}

	//--------------------------------------------------------------------------------------
	// Normal Distribution Function
	//--------------------------------------------------------------------------------------
	float GGX(float ndotwh, float alphaSq)
	{
		if(ndotwh == 0)
			return 0;

		float denom = ndotwh * ndotwh * (alphaSq - 1.0f) + 1.0f;
		return alphaSq / max(PI * denom * denom, 1e-8);
	}

	//--------------------------------------------------------------------------------------
	// Smith Geometry Masking Functions
	//--------------------------------------------------------------------------------------
	// Returns G1(GGXLambda(a)). G1(wo or wi) is the Smith masking function.
	//
	// theta				= wi or wo
	// ndotx				= ndotwi or ndotwo
	// a(costheta, alpha)	= costheta / (alpha * sqrt(1 - costheta * costheta))
	// GGXLambda(a)			= (-1 + sqrt(1 + (1 / (a * a))) / 2
	// G1(GGXLambda)		= 1 / (1 + GGXLambda)
	float SmithG1ForGGX(float alphaSq, float ndotx)
	{
		float ndotxSq = ndotx * ndotx;
		return 2.0f / (sqrt((alphaSq * (1.0f - ndotxSq) / ndotxSq) + 1.0f) + 1.0f);
	}

	// G2 is the height-correlated Smith shadowing-masking function.
	//
	// G2			= 1 / (1 + SmithG1ForGGX(ndotwo) + SmithG1ForGGX(ndotwi))
	// Returns		= G2 / (4.0 * ndotwi * ndotwo)
	// Note that (4.0 * ndotwi * ndotwo) is moved from BRDF to G2 since some terms cancel out.
	// Ref: S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
	float SmithHeightCorrelatedG2ForGGX(float alphaSq, float ndotwi, float ndotwo)
	{
		float GGXLambdaV = ndotwi * sqrt((-ndotwo * alphaSq + ndotwo) * ndotwo + alphaSq);
		float GGXLambdaL = ndotwo * sqrt((-ndotwi * alphaSq + ndotwi) * ndotwi + alphaSq);

		return 0.5f / (GGXLambdaV + GGXLambdaL);
	}

	// Ref: E. Heitz and J. Dupuy, "Implementing a Simple Anisotropic Rough Diffuse Material with Stochastic Evaluation," 2015.
	float SmithHeightCorrelatedG2OverG1(float alphaSq, float ndotwi, float ndotwo)
	{
		float G1wi = SmithG1ForGGX(alphaSq, ndotwi);
		float G1wo = SmithG1ForGGX(alphaSq, ndotwo);

		return G1wi / (G1wi + G1wo - G1wi * G1wo);
	}

	// Samples half vector in a coordinate system where z is aligned with shading normal.
	// PDF is D_Ve(Ne) = G1(wo) * max(0, dot(wo, Ne)) * D(Ne) / wo.z.
	float3 SampleGGXVNDF(float3 wo, float alpha_x, float alpha_y, float2 u)
	{
		// Section 3.2: transforming the view direction to the hemisphere configuration
		float3 Vh = normalize(float3(alpha_x * wo.x, alpha_y * wo.y, wo.z));

#if USE_VNDF_SPHERICAL_CAPS == 0
		// Ref: E. Heitz, "Sampling the GGX Distribution of VisibleNormals," Journal of Computer Graphics Techniques, 2018.
		
		// Section 4.1: orthonormal basis (with special case if cross product is zero)
		float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
		float3 T1 = lensq > 0 ? float3(-Vh.y, Vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
		float3 T2 = cross(Vh, T1);

		// Section 4.2: parameterization of the projected area
		float r = sqrt(u.x);
		float phi = TWO_PI * u.y;
		float t1 = r * cos(phi);
		float t2 = r * sin(phi);
		float s = 0.5 * (1.0 + Vh.z);
		t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
		t2 = lerp(sqrt(1.0 - t1 * t1), t2, s);
	
		// Section 4.3: reprojection onto hemisphere
		float3 Nh = t1 * T1 + t2 * T2 + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * Vh;
#else
		// Ref: J. Dupuy and A. Benyoub, "Sampling Visible GGX Normals with Spherical Caps," High Performance Graphics, 2023.

		// sample a spherical cap in (-wi.z, 1]
		float phi = TWO_PI * u.x;
		float z = mad((1.0f - u.y), (1.0f + wo.z), -wo.z);
		float sinTheta = sqrt(clamp(1.0f - z * z, 0.0f, 1.0f));
		float x = sinTheta * cos(phi);
		float y = sinTheta * sin(phi);
		float3 c = float3(x, y, z);
		// compute halfway direction;
		float3 Nh = c + Vh;
#endif

		// Section 3.4: transforming the normal back to the ellipsoid configuration
		float3 Ne = normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0f, Nh.z)));

		return Ne;
	}

	// Approximates how closely specular lobe dominant direction is aligned with reflected
	// direction. Dominant direction tends to shift away from reflected direction as roughness
	// or view angle increases (off-specular peak).
	// Ref: S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
	float SpecularBRDFGGXSmithDominantFactor(float ndotwo, float roughness)
	{
		float a = 0.298475f * log(39.4115f - 39.0029f * roughness);
		float f = pow(1 - ndotwo, 10.8649f) * (1 - a) + a;
		
		return saturate(f);
	}

	//--------------------------------------------------------------------------------------
	// Utility structure with data needed for BRDF evaluation
	//--------------------------------------------------------------------------------------

	struct ShadingData
	{
		static ShadingData Init(float3 shadingNormal, float3 wo, float metallic, float roughness, float3 baseColor)
		{
			ShadingData si;

			si.wo = wo;
			float ndotwo = dot(shadingNormal, wo);
			si.backfacing_wo = ndotwo < 0;
			// Clamp to a small value to avoid division by zero
			si.ndotwo = clamp(ndotwo, 1e-5f, 1.0f);
			si.alpha = roughness * roughness;
			si.diffuseReflectance = baseColor * (1.0f - metallic);
			// = F0 (F0 and F are not needed at the same time)
			si.F = lerp(0.04f.xxx, baseColor, metallic);

			// Specular reflection and microfacet model are different surface reflection
			// models, but both are handled by the microfacet routines below for convenience.
			si.deltaNDF = roughness <= 1e-2;
			
			return si;
		}

		void SetWi(float3 wi, float3 shadingNormal)
		{
			float3 wh = normalize(wi + this.wo);
			this.ndotwh = saturate(dot(shadingNormal, wh));
			float ndotwi = dot(shadingNormal, wi);
			this.backfacing_wi = ndotwi <= 0;
			// Clamp to a small value to avoid division by zero
			this.ndotwi = clamp(ndotwi, 1e-5f, 1.0f);
			float whdotwo = saturate(dot(wh, this.wo)); // = whdotwi
			this.F = FresnelSchlick(this.F, whdotwo);
		}

		bool IsMetallic()
		{
			return dot(this.diffuseReflectance, 1) == 0;
		}

		// Roughness textures actually contain an "interface" value of roughness that is perceptively 
		// linear. That value needs to be remapped to the alpha value that's used in BRDF equations. 
		// Literature suggests alpha = roughness^2.
		float alpha;

		float3 wo;
		float ndotwi;
		float ndotwo;
		float ndotwh;
		float3 diffuseReflectance;
		float3 F;
		bool deltaNDF;
		bool backfacing_wo;
		bool backfacing_wi;
	};

	//--------------------------------------------------------------------------------------
	// Lambertian
	//--------------------------------------------------------------------------------------

	// Note that multiplication by ndotwi is not part of the Lambertian BRDF and is included for convenience.
	float3 LambertianBRDF(ShadingData surface)
	{
		return (surface.diffuseReflectance * ONE_OVER_PI) * surface.ndotwi;
	}

	// Pdf of cosine-weighted hemisphere sampling
	float LambertianBRDFPdf(ShadingData surface)
	{
		return surface.ndotwi * ONE_OVER_PI;
	}

	// Lambertain BRDF times ndotwi divided by pdf of cosine-weighted hemisphere sampling
	float3 LambertianBrdfDivPdf(ShadingData surface)
	{
		return surface.diffuseReflectance;
	}

	float3 SampleLambertianBrdf(float3 normal, float2 u, out float pdf)
	{
		float3 wiLocal = Sampling::SampleCosineWeightedHemisphere(u, pdf);
		
		float3 T;
		float3 B;
		Math::Transform::revisedONB(normal, T, B);
		float3 wiWorld = wiLocal.x * T + wiLocal.y * B + wiLocal.z * normal;

		return wiWorld;
	}	

	//--------------------------------------------------------------------------------------
	// Microfacet Model
	//--------------------------------------------------------------------------------------

	float3 SpecularBRDFGGXSmith(ShadingData surface)
	{
		if(surface.deltaNDF)
		{
			// Divide by ndotwi is so that integrating brdf over hemisphere would give F (Frensel).
			return surface.F * (surface.ndotwh >= MIN_N_DOT_H_PERFECT_SPECULAR) / surface.ndotwi;
		}
		
		float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
		float NDF = GGX(surface.ndotwh, alphaSq);
		float G2DivDenom = SmithHeightCorrelatedG2ForGGX(alphaSq, surface.ndotwi, surface.ndotwo);

		return surface.F * NDF * G2DivDenom * surface.ndotwi;
	}

	// Evaluates distribution of visible normals (given outgoing dir. wo)
	//		vndf(wh) = GGX(wh) * hdotwo * G1(ndotwo) / ndotwo.
	//
	// After correction for change of variable from wh to wi, it becomes
	//		vndf(wi) = vndf(wh) * 1 / (4 * hdotwi)
	//               = GGX(wh) * G1(ndotwo) / (4 * ndotwo)
	// (Note that hdotwo = hdotwi).
	float GGXVNDFReflectionPdf(ShadingData surface)
	{
		if(surface.deltaNDF)
		{
			// Divide by ndotwi is so that integrating BRDF over hemisphere would give F (Frensel).
		 	return (surface.ndotwh >= MIN_N_DOT_H_PERFECT_SPECULAR) / surface.ndotwi;
		}

		float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
		float NDF = GGX(surface.ndotwh, alphaSq);
		float G1 = SmithG1ForGGX(alphaSq, surface.ndotwo);
		float pdf = (NDF * G1) / (4.0f * surface.ndotwo);
	
		return pdf;
	}

	float3 SampleSpecularMicrofacet(ShadingData surface, float3 shadingNormal, float2 u)
	{
		// Fast path for mirror surfaces
		if(surface.deltaNDF)
			return reflect(-surface.wo, shadingNormal);

		// Build an orthonormal basis C around the normal such that it points towards +Z.
		float3 b1;
		float3 b2;
		Math::Transform::revisedONB(shadingNormal, b1, b2);
		
		// Transform wo from world space to C. M = [b1 b2 n] goes from C to world space, so 
		// we need its inverse. Since M is an orthogonal matrix, its inverse is just its transpose.
		float3x3 worldToLocal = float3x3(b1, b2, shadingNormal);
		float3 woLocal = mul(worldToLocal, surface.wo);

		float3 whLocal = SampleGGXVNDF(woLocal, surface.alpha, surface.alpha, u);
		
		// Go from local space back to world space.
		float3 wh = whLocal.x * b1 + whLocal.y * b2 + whLocal.z * shadingNormal;
		
		// Reflect wo about the plane with normal wh (each microsurface is a perfect mirror).
		float3 wi = reflect(-surface.wo, wh);
	
		return wi;
	}
	
	float3 SpecularMicrofacetGGXSmithDivPdf(ShadingData surface)
	{
		if(surface.deltaNDF)
			return surface.F / surface.ndotwi;

		// When VNDF is used for sampling the incident direction (wi), the expression 
		//		f(wi, wo) * cos(theta) / Pdf(wi)
		// is simplified to F * G2 / G1.
		float alphaSq = max(1e-5f, surface.alpha * surface.alpha);
		return surface.F * SmithHeightCorrelatedG2OverG1(alphaSq, surface.ndotwi, surface.ndotwo);
	}

	//--------------------------------------------------------------------------------------
	// Combined surface BRDFs
	//--------------------------------------------------------------------------------------

	// diffuse plus specular BRDFs times ndotwi at given surface point
	float3 CombinedBRDF(ShadingData surface)
	{
		if (surface.backfacing_wi || surface.backfacing_wo)
			return 0.0;

		float3 specularBrdf = SpecularBRDFGGXSmith(surface);
		float3 diffuseBrdf = (1.0f - surface.F) * LambertianBRDF(surface);

		return diffuseBrdf + specularBrdf;
	}
}

#endif