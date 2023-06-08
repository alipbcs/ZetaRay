// Refs:
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
// visible area of each microsurface.
//
// Therefore, macrosurface BRDF is the integral of contributions of scatterd lights from 
// individual visible microsurfaces where each microsurface scatters light according to its 
// microsurface BRDF (assumed to be perfect mirror). 	
//
// Solving above gives the macrosruface BRDF as:
//		f(p, wi, wo) = (F(wo, h) * NDF(h) * G(wi, wo, h)) / (4 * |n.wi| * |n.wo|)
//
//		where h is the half vector = normalize(wi + wo)
//
// Finally, a choice for NDF and G functions needs to be made (G is dependent on chosen NDF).
// Here GGX normal distribution with height-correlated Smith shadowing-masking function
// is used.

#ifndef BRDF_H
#define BRDF_H

#include "Math.hlsli"
#include "Sampling.hlsli"

namespace BRDF
{
	//--------------------------------------------------------------------------------------
	// Fresnel
	//--------------------------------------------------------------------------------------
	// Note: Schlick's original approximation:
	//		R(theta) = R0 + (1 - R0)(1 - cos(theta))^5
	// Gives reflectivity for a given wavelength (a scalar) given the following assumptions:
	//	1. Surface is dielectric.
	//	2. eta1 (incoming material) < eta2 (outgoing material) (from less dense to denser).
	//
	//	Also when eta1 > eta2, total internal reflection needs to be accounted for, which
	//	happens at incident angle arcsin(eta2 / eta1)
	//--------------------------------------------------------------------------------------
	float3 FresnelSchlick(float3 F0, float whdotwo)
	{
		float tmp = 1.0f - whdotwo;
		return F0 + (1.0f - F0) * tmp * tmp * tmp * tmp * tmp;
	}

	//--------------------------------------------------------------------------------------
	// Normal Distribution Function
	//--------------------------------------------------------------------------------------
	float GGX(float ndotwh, float alphaSq)
	{
		float denom = ndotwh * ndotwh * (alphaSq - 1.0f) + 1.0f;
		return alphaSq / (PI * denom * denom);
	}

	//--------------------------------------------------------------------------------------
	// Smith Geometry Masking Functions
	//--------------------------------------------------------------------------------------
	// G1(wo or wi) is the masking function
	// theta (outgoing direction)	= wi or wo
	// costheta						= ndotwi or ndotwo
	// a(costheta, alpha)			= costheta / (alpha * sqrt(1 - costheta * costheta))
	// GGXLambda(a)					= (-1 + sqrt(1 + (1 / (a * a))) / 2
	// G1(GGXLambda)				= 1 / (1 + GGXLambda)
	// Returns G1(GGXLambda(a))
	float SmithG1ForGGX(float alphaSq, float cosTheta)
	{
		float cosTheta2 = cosTheta * cosTheta;
		return 2.0f / (sqrt((alphaSq * (1.0f - cosTheta2) / cosTheta2) + 1.0f) + 1.0f);
	}

	// Ref: S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering," 2014.
	// G2 is the shadowing-masking function
	// (4.0 * ndotl * ndotv) is moved from BRDF to G2 since it can be simplified
	// G2			= 1 / (1 + SmithG1ForGGX(theta = v) + SmithG1ForGGX(theta = l))
	// Returns		= G2 / (4.0 * ndotl * ndotv)
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

	// Ref: E. Heitz, "Sampling the GGX Distribution of VisibleNormals," Journal of Computer Graphics Techniques, 2018.
	// Samples half vector in a coordinates system where z is aligned with shading normal
	// Input wo: view direction
	// Input alpha_x, alpha_y: roughness parameters
	// Input u: uniform random numbers
	// Output Ne: normal sampled with PDF D_Ve(Ne) = G1(wo) * max(0, dot(wo, Ne)) * D(Ne) / wo.z
	float3 SampleGGXVNDF(float3 wo, float alpha_x, float alpha_y, float2 u)
	{
		// Section 3.2: transforming the view direction to the hemisphere configuration
		float3 Vh = normalize(float3(alpha_x * wo.x, alpha_y * wo.y, wo.z));
	
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
	// Utitlity structure that contains all the surface-related data
	//--------------------------------------------------------------------------------------

	struct SurfaceInteraction
	{
		// BRDF(w_i, w_o) indicates how incoming light from direction w_i is scattered towards direction 
		// w_o (both w_i and w_o point out from the surface). Filling the related data for BRDF computations 
		// is done in two steps as w_i isn't usually known until later on
		
		static SurfaceInteraction InitPartial(float3 sn, float roughness, float3 wo)
		{
			SurfaceInteraction si;
			
			si.wo = wo;
			si.shadingNormal = sn;
			si.ndotwo = clamp(dot(sn, wo), 1e5f, 1.0f);
			si.alpha = roughness * roughness;
			si.alphaSq = max(1e-5f, si.alpha * si.alpha);
			si.DeltaNDF = roughness < 0.01f;
			
			return si;
		}
			
		void InitComplete(float3 wi, float3 baseColor, float metalness)
		{
			float3 wh = normalize(wi + this.wo);	
			this.ndotwh = saturate(dot(this.shadingNormal, wh));			
			this.ndotwi = saturate(dot(this.shadingNormal, wi));
			
			this.diffuseReflectance = baseColor * (1.0f - metalness);
			
			float3 F0 = lerp(0.04f.xxx, baseColor, metalness);
			this.whdotwo = saturate(dot(wh, this.wo)); // == whdotwi
			this.F = FresnelSchlick(F0, this.whdotwo);
		}
	
		float3 shadingNormal;
	
		// Apparently artists work with an "interface" value of roughness that is perceptively linear 
		// i.e. what's in a roughness texture. That value needs to be remapped to the alpha value 
		// that's used in BRDF equations as (artist) roughness != alpha. Literature suggests 
		// alpha = (artist) roughness^2, which means artists are setting the square root of alpha.
		//float roughness;
		float alpha;
	
		// various equations (GGX, lambda, etc) contain the alpha squared term, precalculate it beforehand
		float alphaSq;
	
		float3 wo;
		float ndotwi; // wi is either sampled by BRDF sampling methods or directly provided
		float ndotwo;
		float ndotwh;
		float whdotwo;
		float3 diffuseReflectance;
		float3 F;
		bool DeltaNDF;
	};

	//--------------------------------------------------------------------------------------
	// Lambertian
	//--------------------------------------------------------------------------------------

	float3 LambertianBRDF(float3 diffuseReflectance, float ndotwi)
	{
		return diffuseReflectance * (ndotwi * ONE_OVER_PI);
	}

	half3 LambertianBRDF(half3 diffuseReflectance, half3 ndotwi)
	{
		return diffuseReflectance * ndotwi * half(ONE_OVER_PI);
	}

//	float3 LambertianBrdfDivPdf(SurfaceInteraction surface)
//	{
//		return surface.diffuseReflectance;
//	}

	float3 SampleLambertianBrdf(float3 shadingNormal, float2 u, out float pdf)
	{
		// build rotation quaternion that maps y = (0, 1, 0) to the shading normal
		float4 q = Math::Transform::QuaternionFromY(shadingNormal);
		float3 wiLocal = Sampling::SampleCosineWeightedHemisphere(u, pdf);

		// transform wi from local space to world space
		float3 wiWorld = Math::Transform::RotateVector(wiLocal, q);
	
		return wiWorld;
	}
	
	//--------------------------------------------------------------------------------------
	// Specular Microfacet Model
	//--------------------------------------------------------------------------------------

	float3 SpecularBRDFGGXSmith(SurfaceInteraction surface)
	{
		float NDF = surface.DeltaNDF ? surface.ndotwh >= 0.99f : GGX(surface.ndotwh, max(1e-5f, surface.alphaSq));
		float G2Div4NdotLNdotV = SmithHeightCorrelatedG2ForGGX(surface.alphaSq, surface.ndotwi, surface.ndotwo);

		return surface.F * NDF * G2Div4NdotLNdotV * surface.ndotwi;
	}

	// Evaluates distribution of visible normals (given outgoing dir. wo):
	//		vndf(wh) = GGX(wh) * hdotwo * G1(ndotwo) / ndotwo
	// after correction for change of variable from wh to wi, it becomes
	//		vndf(wi) = vndf(wh) * 1 / (4 * hdotwi)
	//               = GGX(wh) * G1(ndotwo) / (4 * ndotwo)
	// Note that hdotwo = hdotwi.
	float SpecularBRDFGGXSmithPdf(SurfaceInteraction surface)
	{
		float NDF = surface.DeltaNDF ? surface.ndotwh >= 0.99f : GGX(surface.ndotwh, max(1e-5f, surface.alphaSq));
		float G1 = SmithG1ForGGX(surface.alphaSq, surface.ndotwo);

		float pdf = (NDF * G1) / (4.0f * saturate(surface.ndotwo));
	
		return pdf;
	}

	float3 SampleSpecularBRDFGGXSmith(SurfaceInteraction surface, float2 u)
	{		
		// build an orthonormal coord. system C around the surface normal such that it points towards +Z
		float3 b1;
		float3 b2;
		Math::Transform::revisedONB(surface.shadingNormal, b1, b2);
		
		// transform wo from world space to C
		// M = [b1 b2 n] goes from C to world space, so we need its inverse. Since
		// M is an orthogonal matrix, its inverse is just its tranpose
		float3x3 worldToLocal = float3x3(b1, b2, surface.shadingNormal);
		float3 woLocalLH = mul(worldToLocal, surface.wo);
		
		// Transformations between the VNDF space (right handed with +Z as up) and C (left handed with 
		// +Z as up) can be performed using the following:
		//
		//			         | 1  0  0 |                                   
		//		 T_VndfToC = | 0 -1  0 |,	 T_CToVndf = (T_VndfToC)^-1 = (T_VndfToC)^T = T_VndfToC
		//                   | 0  0  1 | 
		float3 woLocalRH = float3(woLocalLH.x, -woLocalLH.y, woLocalLH.z);
		float3 whLocalRH = SampleGGXVNDF(woLocalRH, surface.alpha, surface.alpha, u);
		
		// now reverse the transformations in a LIFO order
		float3 whLocalLH = float3(whLocalRH.x, -whLocalRH.y, whLocalRH.z);
		float3 whWorld = whLocalLH.x * b1 + whLocalLH.y * b2 + whLocalLH.z * surface.shadingNormal;
		
		// reflect wo about the plane with normal wh (each microsurface is a perfect mirror)
		float3 wi = reflect(-surface.wo, whWorld);
	
		return wi;
	}
	
	// When VNDF is used for sampling the incident direction (wi), the expression 
	//		f(wi, wo) * cos(theta) / Pdf(wi)
	//
	// is simplified to F * G2 / G1.
	float3 SpecularBRDFGGXSmithDivPdf(SurfaceInteraction surface)
	{
		return surface.F * SmithHeightCorrelatedG2OverG1(surface.alphaSq, surface.ndotwi, surface.ndotwo);
	}

	//--------------------------------------------------------------------------------------
	// Combined surface BRDFs
	//--------------------------------------------------------------------------------------

	// Computes combined (diffuse + specular BRDF) * ndotwi at given surface point
	float3 ComputeSurfaceBRDF(SurfaceInteraction surface)
	{
		// For reflection to be non-zero, wi and wo have to be in the same
		// hemisphere w.r.t. geometric normal of point being shaded. Otherwise, either
		// a) light is arriving from behind the surface point, or
		// b) viewer is behind the surface and is blocked from seeing the surface point
		//
		// Note that there's a discrepancy between geometric and shading normal, as
		// shading normal (which could be bump mapped) might not reflect the correct
		// orientation of surface and therefore geometric normal should be used

		//	if (!surfaceInteration.wiAndwoInSameHemisphere)
		//		return float3(0.0f, 0.0f, 0.0f);
	
		if (surface.ndotwi <= 0.0f || surface.ndotwo <= 0.0f)
			return 0.0.xxx;
	
		float3 specularBrdf = SpecularBRDFGGXSmith(surface);
		float3 diffuseBrdf = (1.0f.xxx - surface.F) * LambertianBRDF(surface.diffuseReflectance, surface.ndotwi);

		return diffuseBrdf + specularBrdf;
	}
}

#endif