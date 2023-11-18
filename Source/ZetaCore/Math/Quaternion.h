#pragma once

#include "VectorFuncs.h"
#include "Matrix.h"

namespace ZetaRay::Math
{
	// Returns a roations quaternion that can be used to rotate about axis n by angle theta
	ZetaInline __m128 __vectorcall rotationQuat(float3 n, float theta)
	{
		const float s = sinf(0.5f * theta);
		const float c = cosf(0.5f * theta);

		// reminder: sign of cos(theta) won't be taken into account by the following:
		// const float c = sqrtf(1.0f - s * s);

		float4a axis(n, 1.0f);
		float4a sc(s, s, s, c);

		__m128 vAxis = _mm_load_ps(reinterpret_cast<float*>(&axis));
		__m128 vSC = _mm_load_ps(reinterpret_cast<float*>(&sc));
		__m128 vQ = _mm_mul_ps(vAxis, vSC);

		return vQ;
	}

	ZetaInline __m128 __vectorcall quatToAxisAngle(__m128 vQuat)
	{
		float4a quat = store(vQuat);
		float theta = 2.0f * acosf(quat.w);

		__m128 vAxisAngle = normalize(_mm_insert_ps(vQuat, vQuat, 0x8));
		vAxisAngle = _mm_insert_ps(vAxisAngle, _mm_set1_ps(theta), 0x30);
			
		return vAxisAngle;
	}

	ZetaInline void __vectorcall quatToAxisAngle(__m128 vQuat, float3& axis, float& angle)
	{
		float4a quat = store(vQuat);
		angle = 2.0f * acosf(quat.w);
		axis = float3(quat.x, quat.y, quat.z);
		axis.normalize();
	}

	ZetaInline void __vectorcall quatToAxisAngle(float4a& quat, float3& axis, float& angle)
	{
		angle = 2.0f * acosf(quat.w);
		axis = float3(quat.x, quat.y, quat.z);
		axis.normalize();
	}

	// Multiplies two given quaternions
	ZetaInline __m128 __vectorcall mulQuat(const __m128 vP, const __m128 vQ)
	{
		// p = (p1, p2, p3, p4)
		// q = (q1, q2, q3, q4)
		// 
		//			           [ p4  p3 -p2 -p1]
		//	pq = [q1 q2 q3 q4] [-p3  p4  p1 -p2]
		//			           [p2  -p1  p4 -p3]
		//			           [p1   p2  p3  p4]

		const __m128 vMinP = negate(vP);

		// (p1, -p1, p2, -p2)
		const __m128 vTemp0 = _mm_unpacklo_ps(vP, vMinP);
		// (-p3, p3, -p4, p4)
		const __m128 vTemp1 = _mm_unpackhi_ps(vMinP, vP);

		__m128 result = _mm_mul_ps(_mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(3, 3, 3, 3)), vP);
		const __m128 vRow0 = _mm_shuffle_ps(vP, vMinP, V_SHUFFLE_XYZW(3, 2, 1, 0));
		result = _mm_fmadd_ps(_mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(0, 0, 0, 0)), vRow0, result);
		const __m128 vRow1 = _mm_shuffle_ps(vTemp1, vTemp0, V_SHUFFLE_XYZW(0, 3, 0, 2));
		result = _mm_fmadd_ps(_mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(1, 1, 1, 1)), vRow1, result);
		const __m128 vRow2 = _mm_shuffle_ps(vTemp0, vTemp1, V_SHUFFLE_XYZW(2, 1, 3, 0));
		result = _mm_fmadd_ps(_mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(2, 2, 2, 2)), vRow2, result);

		return result;
	}

	ZetaInline __m128 __vectorcall slerp(const __m128 vQ1, const __m128 vQ2, float t)
	{
		// Rotation by unit quaternions q or -q gives the same result, with the difference
		// that q rotates about axis n by angle theta, whereas -q rotates about angle -n by
		// angle 2 * pi - theta. Dot product of two quaternions can be used to check if they are
		// on the same hemisphere. If on opposite hemispheres, negate one of them.
		const __m128 vOne = _mm_set1_ps(1.0f);
		const __m128 vT = _mm_set1_ps(t);
		__m128 vCosTheta = _mm_dp_ps(vQ1, vQ2, 0xff);
		const __m128 vOnSameHemisphere = _mm_cmpgt_ps(vCosTheta, _mm_setzero_ps());
		vCosTheta = _mm_blendv_ps(negate(vCosTheta), vCosTheta, vOnSameHemisphere);

		__m128 vSinTheta = _mm_sub_ps(vOne, _mm_mul_ps(vCosTheta, vCosTheta));
		vSinTheta = _mm_sqrt_ps(vSinTheta);

		__m128 vTheta = acos(vCosTheta);
		__m128 vSinArgs = _mm_insert_ps(_mm_sub_ps(vOne, vT), vT, 0x50);
		vSinArgs = _mm_mul_ps(vSinArgs, vTheta);
		__m128 vSin = sin(vSinArgs);

		__m128 vResSlerp = _mm_mul_ps(vQ1, _mm_shuffle_ps(vSin, vSin, V_SHUFFLE_XYZW(0, 0, 0, 0)));
		__m128 vS2 = _mm_shuffle_ps(vSin, vSin, V_SHUFFLE_XYZW(1, 1, 1, 1));

		vS2 = _mm_blendv_ps(negate(vS2), vS2, vOnSameHemisphere);
		vResSlerp = _mm_fmadd_ps(vQ2, vS2, vResSlerp);
		vResSlerp = _mm_div_ps(vResSlerp, vSinTheta);

		// If theta is near zero, use linear interpolation followed by normalization,
		// otherwise, there might be a divide-by-zero.
		const __m128 vOneMinEps = _mm_set1_ps(1.0f - FLT_EPSILON);
		const __m128 vIsThetaNearZero = _mm_cmpgt_ps(vCosTheta, vOneMinEps);
		const __m128 vResLerp = normalizeFast(lerp(vQ1, vQ2, t));

		__m128 vRes = _mm_blendv_ps(vResSlerp, vResLerp, vIsThetaNearZero);

		return vRes;
	}

	/* TODO
	// pitch: angle of rotation around the x-axis (radians)
	// yaw: angle of rotation around the y-axis (radians)
	// roll: angle of rotation around the z-axis (radians)
	// order is roll, pitch, then yaw
	ZetaInline __m128 __vectorcall rotationQuatFromRollPitchYaw(float pitch, float yaw, float roll)
	{
		// roll:
		//		n = (0, 0, 1) -> q_r = (0, 0, sin(r/2), cos(r/2)
		// pitch:
		//		n = (1, 0, 0) -> q_p = (sin(p/2), 0, 0, cos(p/2)
		// yaw:
		//		n = (0, 1, 0) -> q_y = (0, sin(y/2), 0, cos(y/2)
	}
	*/
}