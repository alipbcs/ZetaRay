#pragma once

#include "Vector.h"

//--------------------------------------------------------------------------------------
// Vector Functions
//--------------------------------------------------------------------------------------

namespace ZetaRay::Math
{
	ZetaInline __m128 __vectorcall abs(const __m128 v)
	{
		// all bits are 0 except for the sign bit
		const __m128 vMinusZero = _mm_set1_ps(-0.0f);

		// set the sign bit to 0
		return _mm_andnot_ps(vMinusZero, v);
	}

	ZetaInline __m256 __vectorcall abs(const __m256 v)
	{
		// all bits are 0 except for the sign bit
		const __m256 vMinusZero = _mm256_set1_ps(-0.0f);

		// set the sign bit to 0
		return _mm256_andnot_ps(vMinusZero, v);
	}

	ZetaInline __m128 __vectorcall negate(const __m128 v)
	{
		// all bits are 0 except for the sign bit
		const __m128 vMinusZero = _mm_set1_ps(-0.0f);

		// set the sign bit to 0
		return _mm_xor_ps(vMinusZero, v);
	}

	// Returns v1 + t * (v2 - v1)
	ZetaInline __m128 __vectorcall lerp(const __m128 v1, const __m128 v2, float t)
	{
		__m128 vT = _mm_broadcast_ss(&t);
		__m128 vInterpolated = _mm_fmadd_ps(vT, _mm_sub_ps(v2, v1), v1);

		return vInterpolated;
	}

	ZetaInline __m128 __vectorcall lerp(const __m128 v1, const __m128 v2, __m128 vT)
	{
		__m128 vInterpolated = _mm_fmadd_ps(vT, _mm_sub_ps(v2, v1), v1);

		return vInterpolated;
	}

	ZetaInline __m128 __vectorcall length(const __m128 v)
	{
		__m128 vNorm2 = _mm_dp_ps(v, v, 0xff);
		__m128 vNorm = _mm_sqrt_ps(vNorm2);

		return vNorm;
	}

	ZetaInline __m128 __vectorcall normalize(const __m128 v)
	{
		__m128 vNorm2 = _mm_dp_ps(v, v, 0xff);
		__m128 vN = _mm_div_ps(v, _mm_sqrt_ps(vNorm2));

		return vN;
	}

	ZetaInline __m128 __vectorcall normalizeFast(const __m128 v)
	{
		__m128 vNorm2 = _mm_dp_ps(v, v, 0xff);
		__m128 vN = _mm_mul_ps(v, _mm_rsqrt_ps(vNorm2));

		return vN;
	}

	ZetaInline bool __vectorcall equal(const __m128 v1, const __m128 v2)
	{
		const __m128 vEps = _mm_set1_ps(FLT_EPSILON);
		__m128 vRes = _mm_cmpgt_ps(vEps, abs(_mm_sub_ps(v1, v2)));
		int r = _mm_movemask_ps(vRes);

		return r == 0;
	}

	ZetaInline __m128 __vectorcall cross(const __m128 v1, const __m128 v2)
	{
		__m128 vTmp0 = _mm_shuffle_ps(v1, v1, 0x9);		// yzx
		__m128 vTmp1 = _mm_shuffle_ps(v2, v2, 0x12);	// zxy

		__m128 uCrossv = _mm_mul_ps(vTmp0, vTmp1);

		vTmp0 = _mm_shuffle_ps(v1, v1, 0x12);	// zxy
		vTmp1 = _mm_shuffle_ps(v2, v2, 0x9);	// yzx

		uCrossv = _mm_sub_ps(uCrossv, _mm_mul_ps(vTmp0, vTmp1));

		// zero out the last element
		return _mm_blend_ps(uCrossv, _mm_setzero_ps(), 0x8);
	}

	ZetaInline __m128 __vectorcall saturate(__m128 v)
	{
		__m128 vOne = _mm_set1_ps(1.0f);
		__m128 vS = _mm_max_ps(v, _mm_setzero_ps());
		vS = _mm_min_ps(vS, vOne);

		return vS;
	}

	ZetaInline __m128 __vectorcall sign(__m128 v)
	{
		__m128 vOnePos = _mm_set1_ps(1.0f);
		__m128 vOneNeg = _mm_set1_ps(-1.0f);
		__m128 vSign = _mm_blendv_ps(vOneNeg, vOnePos, _mm_cmpge_ps(v, _mm_setzero_ps()));

		return vSign;
	}

	// Returns v.x + v.z + v.z in the 1st element of output.
	// Note: assumes fourth of element of v is zero.
	ZetaInline __m128 __vectorcall hadd_float3(__m128 v)
	{
		__m128 vZ = _mm_movehl_ps(v, v);
		__m128 vT = _mm_add_ps(v, vZ);
		__m128 vSum = _mm_add_ss(vT, _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(1, 0, 0, 0)));

		return vSum;
	}	
	
	ZetaInline __m128 __vectorcall encode_octahedral(__m128 v)
	{
		__m128 vAbs = abs(v);
		vAbs = hadd_float3(vAbs);
		vAbs = _mm_shuffle_ps(vAbs, vAbs, V_SHUFFLE_XYZW(0, 0, 0, 0));
		__m128 vEncodedPosZ = _mm_div_ps(v, vAbs);

		__m128 vOne = _mm_set1_ps(1.0f);
		__m128 vSign = sign(v);
		__m128 vEncoded_yx = abs(_mm_shuffle_ps(vEncodedPosZ, vEncodedPosZ, V_SHUFFLE_XYZW(1, 0, 0, 0)));
		__m128 vEncodedNegZ = _mm_sub_ps(vOne, vEncoded_yx);
		vEncodedNegZ = _mm_mul_ps(vEncodedNegZ, vSign);

		__m128 vZ = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(2, 2, 2, 2));
		__m128 vZIsLeNeg = _mm_cmple_ps(vZ, _mm_setzero_ps());
		__m128 vEncoded = _mm_blendv_ps(vEncodedPosZ, vEncodedNegZ, vZIsLeNeg);

		return vEncoded;
	}

	ZetaInline __m128 __vectorcall decode_octahedral(__m128 u)
	{
		__m128 vAbs = abs(u);
		vAbs = _mm_add_ps(vAbs, _mm_shuffle_ps(vAbs, vAbs, V_SHUFFLE_XYZW(1, 0, 0, 0)));

		__m128 vOne = _mm_set1_ps(1.0f);
		// first two elements are now equal to |u.x| + |u.y|
		__m128 vZ = _mm_sub_ps(vOne, vAbs);
		
		__m128 vPosT = saturate(negate(vZ));
		__m128 vNegT = negate(vPosT);
		__m128 vGt0 = _mm_cmpge_ps(u, _mm_setzero_ps());
		__m128 vDecoded = _mm_blendv_ps(vPosT, vNegT, vGt0);
		vDecoded = _mm_add_ps(u, vDecoded);
		// copy z and zero out the last element
		vDecoded = _mm_insert_ps(vDecoded, vZ, 0x28);
		vDecoded = normalize(vDecoded);

		return vDecoded;
	}

	// Following is ported from DirectXMath (MIT License).
	ZetaInline __m128 __vectorcall acos(const __m128 V)
	{
		__m128 nonnegative = _mm_cmpge_ps(V, _mm_setzero_ps());
		__m128 mvalue = _mm_sub_ps(_mm_setzero_ps(), V);
		__m128 x = _mm_max_ps(V, mvalue);  // |V|

		// Compute (1-|V|), clamp to zero to avoid sqrt of negative number.
		__m128 oneMValue = _mm_sub_ps(_mm_set1_ps(1.0f), x);
		__m128 clampOneMValue = _mm_max_ps(_mm_setzero_ps(), oneMValue);
		__m128 root = _mm_sqrt_ps(clampOneMValue);  // sqrt(1-|V|)

		// Compute polynomial approximation
		const __m128 AC1 = _mm_setr_ps(0.0308918810f, -0.0170881256f, 0.0066700901f, -0.0012624911f);
		__m128 vConstants = _mm_shuffle_ps(AC1, AC1, _MM_SHUFFLE(3, 3, 3, 3));
		__m128 t0 = _mm_mul_ps(vConstants, x);

		vConstants = _mm_shuffle_ps(AC1, AC1, _MM_SHUFFLE(2, 2, 2, 2));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, x);

		vConstants = _mm_shuffle_ps(AC1, AC1, _MM_SHUFFLE(1, 1, 1, 1));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, x);

		vConstants = _mm_shuffle_ps(AC1, AC1, _MM_SHUFFLE(0, 0, 0, 0));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, x);

		const __m128 AC0 = _mm_setr_ps(1.5707963050f, -0.2145988016f, +0.0889789874f, -0.0501743046f);
		vConstants = _mm_shuffle_ps(AC0, AC0, _MM_SHUFFLE(3, 3, 3, 3));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, x);

		vConstants = _mm_shuffle_ps(AC0, AC0, _MM_SHUFFLE(2, 2, 2, 2));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, x);

		vConstants = _mm_shuffle_ps(AC0, AC0, _MM_SHUFFLE(1, 1, 1, 1));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, x);

		vConstants = _mm_shuffle_ps(AC0, AC0, _MM_SHUFFLE(0, 0, 0, 0));
		t0 = _mm_add_ps(t0, vConstants);
		t0 = _mm_mul_ps(t0, root);

		__m128 t1 = _mm_sub_ps(_mm_set1_ps(PI), t0);
		t0 = _mm_and_ps(nonnegative, t0);
		t1 = _mm_andnot_ps(nonnegative, t1);
		t0 = _mm_or_ps(t0, t1);

		return t0;
	}

	// Following is ported from DirectXMath (MIT License).
	// vTheta must be in -XM_PI <= theta < XM_PI
	ZetaInline __m128 __vectorcall sin(__m128 vTheta)
	{
#ifdef _DEBUG
		__m128 vM1 = _mm_cmpge_ps(vTheta, _mm_set1_ps(-PI));
		__m128 vM2 = _mm_cmpgt_ps(_mm_set1_ps(PI), vTheta);
		__m128 vIsThetaValid = _mm_and_ps(vM1, vM2);

		//int valid = _mm_movemask_ps(vIsThetaValid);
		//Assert(valid == 0xf, "Invalid theta value");
#endif // _DEBUG

		// Map in [-pi/2,pi/2] with sin(y) = sin(x).
		__m128 sign = _mm_and_ps(vTheta, _mm_set1_ps(-0.0f));
		__m128 c = _mm_or_ps(_mm_set1_ps(PI), sign);  // pi when x >= 0, -pi when x < 0
		__m128 absx = _mm_andnot_ps(sign, vTheta);  // |x|
		__m128 rflx = _mm_sub_ps(c, vTheta);
		__m128 comp = _mm_cmple_ps(absx, _mm_set1_ps(PI_OVER_2));
		__m128 select0 = _mm_and_ps(comp, vTheta);
		__m128 select1 = _mm_andnot_ps(comp, rflx);
		vTheta = _mm_or_ps(select0, select1);

		__m128 x2 = _mm_mul_ps(vTheta, vTheta);

		// Compute polynomial approximation
		const __m128 SC1 = _mm_setr_ps(-2.3889859e-08f, -0.16665852f, +0.0083139502f, -0.00018524670f);
		__m128 vConstants = _mm_shuffle_ps(SC1, SC1, _MM_SHUFFLE(0, 0, 0, 0));
		__m128 Result = _mm_mul_ps(vConstants, x2);

		const __m128 SC0 = _mm_setr_ps(-0.16666667f, +0.0083333310f, -0.00019840874f, +2.7525562e-06f);
		vConstants = _mm_shuffle_ps(SC0, SC0, _MM_SHUFFLE(3, 3, 3, 3));
		Result = _mm_add_ps(Result, vConstants);
		Result = _mm_mul_ps(Result, x2);

		vConstants = _mm_shuffle_ps(SC0, SC0, _MM_SHUFFLE(2, 2, 2, 2));
		Result = _mm_add_ps(Result, vConstants);
		Result = _mm_mul_ps(Result, x2);

		vConstants = _mm_shuffle_ps(SC0, SC0, _MM_SHUFFLE(1, 1, 1, 1));
		Result = _mm_add_ps(Result, vConstants);
		Result = _mm_mul_ps(Result, x2);

		vConstants = _mm_shuffle_ps(SC0, SC0, _MM_SHUFFLE(0, 0, 0, 0));
		Result = _mm_add_ps(Result, vConstants);
		Result = _mm_mul_ps(Result, x2);
		Result = _mm_add_ps(Result, _mm_set1_ps(1.0f));
		Result = _mm_mul_ps(Result, vTheta);

		return Result;
	}

	ZetaInline float4a __vectorcall store(__m128 v)
	{
		float4a f;
		_mm_store_ps(reinterpret_cast<float*>(&f), v);

		return f;
	}

	ZetaInline float3 __vectorcall storeFloat3(__m128 v)
	{
		float3 f;
		f.x = _mm_cvtss_f32(v);
		f.y = _mm_cvtss_f32(_mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(1, 0, 0, 0)));
		f.z = _mm_cvtss_f32(_mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(2, 0, 0, 0)));

		return f;
	}

	ZetaInline float4 __vectorcall storeFloat4(__m128 v)
	{
		float4 f;
		f.x = _mm_cvtss_f32(v);
		f.y = _mm_cvtss_f32(_mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(1, 0, 0, 0)));
		f.z = _mm_cvtss_f32(_mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(2, 0, 0, 0)));
		f.w = _mm_cvtss_f32(_mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(3, 0, 0, 0)));

		return f;
	}

	ZetaInline __m128 __vectorcall load(float4a& v)
	{
		__m128 vV = _mm_load_ps(reinterpret_cast<float*>(&v));

		return vV;
	}

	ZetaInline __m128 __vectorcall loadFloat2(float2& v)
	{
		// &v does not need to be aligned and the last two elements are set to 0
		__m128 xy = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&v)));
		return xy;
	}

	ZetaInline __m128 __vectorcall loadFloat3(float3& v)
	{
		// &v does not need to be aligned and the last two elements are set to 0
		__m128 xy = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&v)));
		// &v.z does not need to be aligned
		__m128 z = _mm_load_ss(&v.z);
		return _mm_insert_ps(xy, z, 0x20);
	}

	ZetaInline __m128 __vectorcall loadFloat4(float4& v)
	{
		return _mm_loadu_ps(reinterpret_cast<float*>(&v));
	}}