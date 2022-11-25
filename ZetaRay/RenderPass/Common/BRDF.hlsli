// Refs:
// 1. M. Pharr, W. Jakob, and G. Humphreys, Physically Based Rendering: From theory to implementation, Morgan Kaufmann, 2016.
// 2. E. Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs," Journal of Computer Graphics Techniques, 2014.
// 3. Walter et al., "Microfacet Models for Refraction through Rough Surfaces," in EGSR'07, 2007.
// 4. J. Boksansky, "Crash Course in BRDF Implementation," 2021. [Online]. Available: https://boksajak.github.io/blog/BRDF.
//
// In microfacet models, a macrosurface BRDF is designed to match
// the aggregate behavior of visible micorsurfaces that constitute that macrosurface.
//
// Microsurfaces can be statistically described by a normal distribution function (NDF),
// a shadowing-masking function (G) and a microsurface BRDF that describes how 
// each microsurface scatters light. Commonly, a perfect specular (mirror) BRDF is 
// assumed for microsurfaces. Furthermore, product of NDF and G gives visible area of 
// each microsurface.
//
// Therefore, macrosurface BRDF is the integral of contributions of scatterd lights
// from individual visible microsurfaces where each microsurface scatters light 
// according to its microsurface BRDF (assumed to be perfect mirror). 	
//
// Solving above gives the macrosruface BRDF as:
//		f(p, wi, wo) = (F(wo, h) * NDF(h) * G(wi, wo, h)) / (4 * |n.wi| * |n.wo|)
//
//		where h is the half vector: h = normalize(wi + wo)
//
// Finally, a choice for NDF and G functions needs to be made (G is dependant on chosen NDF).
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
	// Note: original Schlick's approximation:
	//		R(theta) = R0 + (1 - R0)(1 - cos(theta))^5
	// Gives reflectivity for a given wavlength (a scalar) given the following assumptions:
	//	1. Surface is dielectric
	//	2. eta1 (incoming material) < eta2 (outgoing material) (from less dense to denser)
	//	Also when eta1 > eta2, total internal reflection needs to be accounted for, which
	//	happens at incident angle arcsin(eta2 / eta1)
	// 
	//	To account for wavelength dependance of reflectance, a RGB value is used.
	//	Side note: F0 is approximated as:
	//		F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metalneess);
	//		Due to 0.04 factor for RGB channels when material is dielectric, does
	//		that mean wavelentgh dependance for them is igonore?
	//--------------------------------------------------------------------------------------
	float3 FresnelSchlick(float3 F0, float hdotwo)
	{
		float tmp = 1.0f - hdotwo;
		return F0 + (1.0f - F0) * tmp * tmp * tmp * tmp * tmp;
	}

	//--------------------------------------------------------------------------------------
	// Normal Distribution Function
	//--------------------------------------------------------------------------------------
	float GGX(float ndotwh, float alphaSq)
	{
		ndotwh = max(ndotwh, 0.00001f);
		float denom = ndotwh * ndotwh * (alphaSq - 1.0f) + 1.0f;
		return alphaSq / (PI * denom * denom);
	}

	//--------------------------------------------------------------------------------------
	// Smith Geometry Masking Functions
	//--------------------------------------------------------------------------------------
	// G1(wo or wi) is masking function
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
	// G2 is shadowing-masking function
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

		return G1wo / (G1wi + G1wo - G1wi * G1wo);
	}

	//	Conventions
	//
	//	float3 *BRDF(...)			-> computes BRDF * ndtol (ndtol is not part of BRDF, but including it here from 
	//								scattering eq. simplifies calculations)
	//	float3 Sample*Brdf(...)		-> returns PDF of given sample
	//	float *BrdfDivPdf(...)		-> When using Monte Carlor estimation for estimating scattering integral, BRDF needs to be sampled
	//								e.g. computing throughput for path integral. Resulting term brdf_sample / pdf sometimes
	//								can be simplified since some terms appear in both BRDF and its pdf.
	//
	//	Sampling methods returns samples in a local coord. sys. L where surface normal is z = (0, 0, 1). So
	//	returned sample has to be transformed into a coordinate system where shading normal is
	//	mapped to z = (0, 0, 1). There are a few ways to do that:
	//	1. Build a change of coordinate matrix M with basis vectors x', y', z' where:
	//			z' = shading normal
	//			x', y' = any two vetors that form a orhtonormal coords system with z'
	//
	//	   Assuming shading normal is in world space, Inv(M) goes from world space to L.
	//
	//	2. Use a rotation matrix with axis cross(shading normal, z) and theta = arccos(dot(shading normal, z))
	//
	//	3. Use a quaternion rotation operator q (shading normal, 0) q* with q = (axis * sin(0.5 * theta), cos(0.5 * theta))
	//	Using approach described here http://lolengine.net/blog/2013/09/18/beautiful-maths-quaternion-from-vectors, 
	//	computing trigonometric functions can be replaced with simpler terms.

	//--------------------------------------------------------------------------------------
	// Utitlity structure that contains all the surface-related data
	//--------------------------------------------------------------------------------------

	struct SurfaceInteraction
	{
		// BRDF is a function of pair of directions w_i and w_o that indicates how incoming light from 
		// direction w_i is scattered towards direction w_o (both w_i and w_o point out from the surface).
		// Filling the related information for BRDF computation is done in two step where following functions
		// fills the members that only depend on w_o
		static SurfaceInteraction InitPartial(float3 sn, float roughness, float metalness,
			float3 wo, float3 baseColor)
		{
			SurfaceInteraction si;
			
			si.wo = wo;
			si.shadingNormal = sn;
			//this.geometricNormal = gn;
			si.ndotwo = clamp(dot(sn, wo), 0.00001f, 1.0f);
			si.alpha = roughness * roughness;
			si.alphaSq = max(0.00001f, si.alpha * si.alpha);
			si.diffuseReflectance = baseColor * (1.0f - metalness);
			si.F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metalness);
			
			return si;
		}
	
		// Completes computation of SurfaceInteractionData after wi has become known
		void InitComplete(float3 wi)
		{
			wh = normalize(wi + wo);
			ndotwi = clamp(dot(shadingNormal, wi), 0.00001f, 1.0f);
			whdotwo = saturate(dot(wh, wo)); // == hdotwi
			ndotwh = saturate(dot(shadingNormal, wh));
			//surfaceInteraction.wiAndwoInSameHemisphere = 
			//	dot(surfaceInteraction.geometricNormal, wi) *
			//	dot(surfaceInteraction.geometricNormal, surfaceInteraction.wo) > 0 ? true : false;
			F = FresnelSchlick(F0, whdotwo);
		}
	
		float3 shadingNormal;
		//float3 geometricNormal;
	
		// Apparently artists work with an "interface" value of roughness
		// that is perceptively linear i.e. what's in a roughness texture. That
		// value needs to be remapped to alpha values that's used in BRDF equations,
		// since (artist) roughness != alpha. Literature suggests alpha = (artist) roughness^2,
		// which means artists are setting square root of alpha.
		//float roughness;
		float alpha;
	
		// various equations (GGX, lambda, ...) have alpha squared in them, which can be precalculated
		float alphaSq;
	
		float3 wo;
		float3 wh;
		float ndotwi; // wi is either sampled by BRDF sampling methods or directly provided
		float ndotwo;
		float whdotwo;
		float ndotwh;
		float3 diffuseReflectance;
		float3 F0;
		float3 F;
		bool wiAndwoInSameHemisphere;
	};

	//--------------------------------------------------------------------------------------
	// Lambertian
	//--------------------------------------------------------------------------------------

	float3 LambertianBRDF(float3 diffuseReflectance, float ndotwi)
	{
		return diffuseReflectance * (ndotwi * ONE_DIV_PI);
	}

	half3 LambertianBRDF(half3 diffuseReflectance, half3 ndotwi)
	{
		return diffuseReflectance * ndotwi * half(ONE_DIV_PI);
	}

	float3 LambertianBrdfDivPdf(SurfaceInteraction surfaceInteraction)
	{
		return surfaceInteraction.diffuseReflectance;
	}

	float3 SampleLambertianBrdf(in float3 shadingNormal, in float2 u, out float pdf)
	{
		// build rotation quaternion that maps shading normal to y = (0, 1, 0)
	//	float3 snCrossY = float3(shadingNormal.z, 0.0f, -shadingNormal.x);
	//	float4 q = float4(snCrossY, 1.0f + dot(shadingNormal, float3(0.0f, 1.0f, 0.0f)));
		float4 q = Math::Transform::QuaternionFromY(shadingNormal);
	
		float3 wiLocal = Sampling::SampleCosineWeightedHemisphere(u, pdf);

		// transform wh from local space to world space
		float3 wiWorld = Math::Transform::RotateVector(wiLocal, q);
	
		return wiWorld;
	}
	
	//--------------------------------------------------------------------------------------
	// Specular Microfacet Model
	//--------------------------------------------------------------------------------------

	float3 SpecularBRDFGGXSmith(SurfaceInteraction surfaceInteraction)
	{
		float NDF = GGX(surfaceInteraction.ndotwh, surfaceInteraction.alphaSq);
		float G2Div4NdotLNdotV = SmithHeightCorrelatedG2ForGGX(surfaceInteraction.alphaSq,
			surfaceInteraction.ndotwi, surfaceInteraction.ndotwo);

		return surfaceInteraction.F * NDF * G2Div4NdotLNdotV * surfaceInteraction.ndotwi;
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

	// Evaluates distribution of visible normals eq.
	//		vndf = GGX(wh) * hdotwi * G1(ndotwo) / ndotwo
	// after correcting for change of dist. from wh to wi, it becomes
	//		vndf * 1 / 4 * hdotwi
	// Note: "Importance Sampling Microfacet-Based BSDFs using the Distribution of Visible Normals" eq (2) 
	// specifies that G1 is a function of wi and wh, but Smith G1 is computed w.r.t. ndotwo. As
	// explained in page 68, "masking function depends only on the variable a = 1 / alpha * tan(theta_0), 
	// where tan(theta_0) is the slope of the outgoing direction."
	// theta_0 is depicted in figure 3, as the angle between surface normal (0, 0, 1) and outgoing direction (view)
	float SpecularBRDFGGXSmithPdf(SurfaceInteraction surfaceInteraction)
	{
		float NDF = GGX(surfaceInteraction.ndotwh, max(0.00001f, surfaceInteraction.alphaSq));
		float G1 = SmithG1ForGGX(surfaceInteraction.alphaSq, surfaceInteraction.ndotwo);

		float pdf = (NDF * G1) / (4.0f * saturate(surfaceInteraction.ndotwo));
	
		return pdf;
	}

	// samples BRDF and fills out rest of SurfaceInteractionData
	float3 SampleSpecularBRDFGGXSmith(SurfaceInteraction surfaceInteraction, float2 u)
	{
		// build rotation quaternion for transforming shading normal to z = (0, 0, 1)
		float3 snCrossZ = float3(surfaceInteraction.shadingNormal.y, -surfaceInteraction.shadingNormal.x, 0.0f);
		float4 q = float4(snCrossZ, 1.0f + dot(surfaceInteraction.shadingNormal, float3(0.0f, 0.0f, 1.0f)));
		float3 woLocal = Math::Transform::RotateVector(surfaceInteraction.wo, q);
	
		float3 whLocal = SampleGGXVNDF(woLocal, surfaceInteraction.alpha, surfaceInteraction.alpha, u);

		// transform wh from local space to world space
		float4 qReverse = q * float4(-1.0f, -1.0f, -1.0f, 1.0f);
		float3 whWorld = Math::Transform::RotateVector(whLocal, qReverse);
		float3 wi = reflect(-surfaceInteraction.wo, whWorld);
	
		return wi;
	}

	float3 SpecularBRDFGGXSmithDivPdf(SurfaceInteraction surfaceInteraction)
	{
		return surfaceInteraction.F * SmithHeightCorrelatedG2OverG1(surfaceInteraction.alphaSq,
		surfaceInteraction.ndotwi, surfaceInteraction.ndotwo);
	}

	//--------------------------------------------------------------------------------------
	// Combined surface BRDFs
	//--------------------------------------------------------------------------------------

	// Computes combined (diffuse + specular BRDF) * ndotwi at given surface point
	float3 ComputeSurfaceBRDF(SurfaceInteraction surfaceInteration)
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
	
		if (surfaceInteration.ndotwi <= 0.0f || surfaceInteration.ndotwo <= 0.0f)
			return float3(0.0f, 0.0f, 0.0f);
	
		float3 specularBrdf = SpecularBRDFGGXSmith(surfaceInteration);
		float3 diffuseBrdf = (1.0f.xxx - surfaceInteration.F) * LambertianBRDF(surfaceInteration.diffuseReflectance, surfaceInteration.ndotwi);

		return diffuseBrdf + specularBrdf;
	}
}

#endif