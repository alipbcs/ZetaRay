#pragma once

#include "Matrix.h"
#include "VectorFuncs.h"

//--------------------------------------------------------------------------------------
// Matrix Functions
//--------------------------------------------------------------------------------------

namespace ZetaRay::Math
{
	__forceinline v_float4x4 zero() noexcept
	{
		const __m256 vZero = _mm256_setzero_ps();

		return v_float4x4(_mm_setzero_ps(),
			_mm_setzero_ps(),
			_mm_setzero_ps(),
			_mm_setzero_ps());
	}

	__forceinline v_float4x4 identity() noexcept
	{
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vOne = _mm_set1_ps(1.0f);

		return v_float4x4(_mm_insert_ps(vZero, vOne, 0x0e),
			_mm_insert_ps(vZero, vOne, 0x1d),
			_mm_insert_ps(vZero, vOne, 0x2b),
			_mm_insert_ps(vZero, vOne, 0x37));
	}

	__forceinline v_float4x4 __vectorcall add(const v_float4x4 M1, const v_float4x4& M2) noexcept
	{
		return v_float4x4(_mm_add_ps(M1.vRow[0], M2.vRow[0]),
			_mm_add_ps(M1.vRow[1], M2.vRow[1]),
			_mm_add_ps(M1.vRow[2], M2.vRow[2]),
			_mm_add_ps(M1.vRow[3], M2.vRow[3]));
	}

	__forceinline v_float4x4 __vectorcall sub(const v_float4x4 M1, const v_float4x4& M2) noexcept
	{
		return v_float4x4(_mm_sub_ps(M1.vRow[0], M2.vRow[0]),
			_mm_sub_ps(M1.vRow[1], M2.vRow[1]),
			_mm_sub_ps(M1.vRow[2], M2.vRow[2]),
			_mm_sub_ps(M1.vRow[3], M2.vRow[3]));
	}

	__forceinline v_float4x4 __vectorcall transpose(const v_float4x4 M) noexcept
	{
		//
		//		0  1  2  3
		// M =	4  5  6  7
		//      8  9  10 11
		//		12 13 14 15
		//
		__m128 vTemp0 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(0, 1, 0, 1));	// 0  1  4  5
		__m128 vTemp1 = _mm_shuffle_ps(M.vRow[2], M.vRow[3], V_SHUFFLE_XYZW(0, 1, 0, 1));	// 8  9  12 13
		__m128 vTemp2 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(2, 3, 2, 3));	// 2  3  6  7
		__m128 vTemp3 = _mm_shuffle_ps(M.vRow[2], M.vRow[3], V_SHUFFLE_XYZW(2, 3, 2, 3));	// 10 11 14 15

		return v_float4x4(_mm_shuffle_ps(vTemp0, vTemp1, 0x88),
			_mm_shuffle_ps(vTemp0, vTemp1, 0xdd),
			_mm_shuffle_ps(vTemp2, vTemp3, 0x88),
			_mm_shuffle_ps(vTemp2, vTemp3, 0xdd));
	}

	__forceinline __m128 __vectorcall mul(const v_float4x4 M, const __m128& v) noexcept
	{
		// (v.x, v.x, v.x, v.x)
		const __m128 vX = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(0, 0, 0, 0));
		__m128 result = _mm_mul_ps(vX, M.vRow[0]);

		// (v.y, v.y, v.y, v.y)
		const __m128 vY = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(1, 1, 1, 1));
		result = _mm_fmadd_ps(vY, M.vRow[1], result);

		// (v.z, v.z, v.z, v.z)
		const __m128 vZ = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(2, 2, 2, 2));
		result = _mm_fmadd_ps(vZ, M.vRow[2], result);

		// (v.w, v.w, v.w, v.w)
		const __m128 vW = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(3, 3, 3, 3));
		result = _mm_fmadd_ps(vW, M.vRow[3], result);

		return result;
	}

	__forceinline v_float4x4 __vectorcall mul(const v_float4x4 M1, const v_float4x4& M2) noexcept
	{
		v_float4x4 M3;

		M3.vRow[0] = mul(M2, M1.vRow[0]);
		M3.vRow[1] = mul(M2, M1.vRow[1]);
		M3.vRow[2] = mul(M2, M1.vRow[2]);
		M3.vRow[3] = mul(M2, M1.vRow[3]);

		return M3;
	}

	__forceinline __m128 __vectorcall determinant3x3(const v_float4x4 M) noexcept
	{
		//  
		//			 a b c
		//	M_3x3 =	 d e f
		//			 g h i
		//
		__m128 vefd = _mm_shuffle_ps(M.vRow[1], M.vRow[1], V_SHUFFLE_XYZW(1, 2, 0, 0));
		__m128 vigh = _mm_shuffle_ps(M.vRow[2], M.vRow[2], V_SHUFFLE_XYZW(2, 0, 1, 0));
		__m128 vTemp0 = _mm_mul_ps(vefd, vigh);

		__m128 vfde = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(2, 0, 1, 0));
		__m128 vhig = _mm_shuffle_ps(M.vRow[2], M.vRow[2], V_SHUFFLE_XYZW(1, 2, 0, 0));
		__m128 vTemp1 = _mm_mul_ps(vfde, vhig);

		vefd = _mm_blend_ps(M.vRow[0], _mm_setzero_ps(), 0x8);
		return _mm_dp_ps(_mm_sub_ps(vTemp0, vTemp1), vefd, 0xff);
	}

	// Given transformation matrix of the following form, returns its inverse,
	// M = S * R * T,
	// where S is a scaling, R is a rotation and T is a translation transformation
	__forceinline v_float4x4 __vectorcall inverseSRT(const v_float4x4 M) noexcept
	{
		const __m128 vOne = _mm_set1_ps(1.0f);
		const __m128 vZero = _mm_setzero_ps();

		//		0  1  2              0  4  8
		// M =	4  5  6 	-->  M = 1  5  9
		//      8  9  10             2  6  10
		const __m128 vTemp0 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(0, 1, 0, 1));	// 0  1  4  5
		const __m128 vTemp1 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(2, 0, 2, 0));	// 2  _  6  _

		// for 3x3 matrix M = [u, v, w] where u,v,w are columns vectors, M^(-1) is given by
		// M = [a b c]^T
		// where 
		//		a = (v * w) / u.(v * w)
		//		b = (w * u) / u.(v * w)
		//		c = (u * v) / u.(v * w)
		// 
		// Reminder: determinant of M is given by scalar triple product:
		// |u v w| = [u v w] = u.(v * w) == v.(w * u) == w.(u * v)

		//  M = [u v w] -> extract u, v, w
		const __m128 u = _mm_shuffle_ps(vTemp0, M.vRow[2], V_SHUFFLE_XYZW(0, 2, 0, 0));
		const __m128 v = _mm_shuffle_ps(vTemp0, M.vRow[2], V_SHUFFLE_XYZW(1, 3, 1, 0));
		const __m128 w = _mm_shuffle_ps(vTemp1, M.vRow[2], V_SHUFFLE_XYZW(0, 2, 2, 0));

		const __m128 vCrossW = cross(v, w);
		const __m128 uDotvCrossW = _mm_dp_ps(u, vCrossW, 0xff);
		const __m128 detRcp = _mm_div_ps(vOne, uDotvCrossW);

		__m128 wCrossu = cross(w, u);
		__m128 uCrossv = cross(u, v);
		const __m128 vTinv = _mm_insert_ps(minus(M.vRow[3]), vOne, 0x30);

		v_float4x4 vI;
		vI.vRow[0] = _mm_insert_ps(_mm_mul_ps(vCrossW, detRcp), vZero, 0x30);
		vI.vRow[1] = _mm_insert_ps(_mm_mul_ps(wCrossu, detRcp), vZero, 0x30);
		vI.vRow[2] = _mm_insert_ps(_mm_mul_ps(uCrossv, detRcp), vZero, 0x30);

		vI.vRow[3] = _mm_mul_ps(_mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(0, 0, 0, 0)), vI.vRow[0]);
		vI.vRow[3] = _mm_fmadd_ps(_mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(1, 1, 1, 1)), vI.vRow[1], vI.vRow[3]);
		vI.vRow[3] = _mm_fmadd_ps(_mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(2, 2, 2, 2)), vI.vRow[2], vI.vRow[3]);
		vI.vRow[3] = _mm_insert_ps(vI.vRow[3], vOne, 0x30);

		return vI;
	}

	__forceinline v_float4x4 scale(float sx, float sy, float sz) noexcept
	{
		float4a f(sx, sy, sz, 1.0f);

		const __m128 vZero = _mm_setzero_ps();
		const __m128 vS = _mm_load_ps(reinterpret_cast<float*>(&f));

		auto fffff = _mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 0, 0, 1));

		return v_float4x4(_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(1, 0, 0, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 1, 0, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 0, 1, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 0, 0, 1)));
	}

	__forceinline v_float4x4 __vectorcall scale(float4a s) noexcept
	{
		s.w = 1.0f;
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vS = _mm_load_ps(reinterpret_cast<float*>(&s));

		return v_float4x4(_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(1, 0, 0, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 1, 0, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 0, 1, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 0, 0, 1)));
	}

	__forceinline v_float4x4 __vectorcall scale(const __m128 vS) noexcept
	{
		const __m128 vZero = _mm_setzero_ps();

		return v_float4x4(_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(1, 0, 0, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 1, 0, 0)),
			_mm_blend_ps(vZero, vS, _MM_BLEND_XYZW(0, 0, 1, 0)),
			_mm_blend_ps(vZero, _mm_set1_ps(1.0f), _MM_BLEND_XYZW(0, 0, 0, 1)));
	}

	__forceinline v_float4x4 __vectorcall rotate(const __m128 vN, float angle) noexcept
	{
		v_float4x4 vR;

		const float c = cosf(angle);
		const float s = sinf(angle);

		const __m128 vC = _mm_set1_ps(c);
		const __m128 v1subC = _mm_set1_ps(1.0f - c);
		const __m128 vS = _mm_set1_ps(s);
		//__m128 vN = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&n)));
		//vN = _mm_insert_ps(vN, _mm_load_ss(&n.z), 0x20);

		__m128 vYZX = _mm_shuffle_ps(vN, vN, V_SHUFFLE_XYZW(1, 2, 0, 0));
		__m128 vTemp0 = _mm_mul_ps(vN, vN);
		vTemp0 = _mm_mul_ps(vTemp0, v1subC);	// ((1 - c)x^2, (1 - c)y^2, (1 - c)z^2)
		__m128 vTemp1 = _mm_mul_ps(vN, vYZX);
		vTemp1 = _mm_mul_ps(vTemp1, v1subC);	// ((1 - c)xy, (1 - c)yz, (1 - c)xz)
		__m128 vTemp2 = _mm_mul_ps(vN, vS);		// (sx, sy, sz)

		__m128 vTemp3 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(2, 0, 1, 0));

		vTemp2 = _mm_sub_ps(vTemp1, vTemp3);	// ((1 - c)xy - sz, (1 - c)yz - sx, (1 - c)xz - sy)
		vTemp3 = _mm_add_ps(vTemp1, vTemp3);	// ((1 - c)xy + sz, (1 - c)yz + sx, (1 - c)xz + sy)
		vTemp0 = _mm_add_ps(vTemp0, vC);		// (c + (1 - c)x^2, c + (1 - c)y^2, c + (1 - c)z^2)
		vTemp1 = _mm_add_ps(vC, v1subC);

		vR.vRow[0] = _mm_insert_ps(_mm_shuffle_ps(vTemp3, vTemp2, V_SHUFFLE_XYZW(0, 0, 2, 2)), vTemp0, 0x8);
		vR.vRow[1] = _mm_insert_ps(_mm_shuffle_ps(vTemp2, vTemp3, V_SHUFFLE_XYZW(0, 0, 1, 1)), vTemp0, 0x58);
		vR.vRow[2] = _mm_insert_ps(_mm_insert_ps(vTemp0, vTemp3, 0x8a), vTemp2, 0x58);
		vR.vRow[3] = _mm_insert_ps(vTemp1, vTemp1, 0x7);

		return vR;
	}

	__forceinline v_float4x4 rotateX(float angle) noexcept
	{
		v_float4x4 vR;

		const float c = cosf(angle);
		const float s = sinf(angle);
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vOne = _mm_set1_ps(1.0f);
		const __m128 vC = _mm_broadcast_ss(&c);
		const __m128 vS = _mm_broadcast_ss(&s);
		const __m128 vMinusS = minus(vS);

		vR.vRow[0] = _mm_insert_ps(vZero, vOne, 0x0);
		vR.vRow[1] = _mm_insert_ps(vC, vS, 0x29);
		vR.vRow[2] = _mm_insert_ps(vC, vMinusS, 0x19);
		vR.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

		return vR;
	}

	__forceinline v_float4x4 rotateY(float angle) noexcept
	{
		v_float4x4 vR;

		const float c = cosf(angle);
		const float s = sinf(angle);
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vOne = _mm_set1_ps(1.0f);
		const __m128 vC = _mm_broadcast_ss(&c);
		const __m128 vS = _mm_broadcast_ss(&s);
		const __m128 vMinusS = minus(vS);

		vR.vRow[0] = _mm_insert_ps(vC, vMinusS, 0x2a);
		vR.vRow[1] = _mm_insert_ps(vZero, vOne, 0x10);
		vR.vRow[2] = _mm_insert_ps(vC, vS, 0xa);
		vR.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

		return vR;
	}

	__forceinline v_float4x4 rotateZ(float angle) noexcept
	{
		v_float4x4 vR;

		const float c = cosf(angle);
		const float s = sinf(angle);
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vOne = _mm_set1_ps(1.0f);
		const __m128 vC = _mm_broadcast_ss(&c);
		const __m128 vS = _mm_broadcast_ss(&s);
		const __m128 vMinusS = minus(vS);

		vR.vRow[0] = _mm_insert_ps(vC, vS, 0x1c);
		vR.vRow[1] = _mm_insert_ps(vC, vMinusS, 0xc);
		vR.vRow[2] = _mm_insert_ps(vZero, vOne, 0x20);
		vR.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

		return vR;
	}

	// Returns a rotation matrix from the given unit quaternion. Assumes vQ is a unit quaternion
	__forceinline v_float4x4 __vectorcall rotationMatrixFromQuat(const __m128 vQ) noexcept
	{
		// (q1^2, q2^2, q3^2, q4^2)
		const __m128 vQ2 = _mm_mul_ps(vQ, vQ);
		const __m128 vMin2 = _mm_set1_ps(-2.0f);
		const __m128 v2 = _mm_set1_ps(2.0f);
		const __m128 vOne = _mm_set1_ps(1.0f);

		// (q3^2, q3^2, q2^2, _)
		__m128 vTemp0 = _mm_shuffle_ps(vQ2, vQ2, V_SHUFFLE_XYZW(2, 2, 1, 0));

		// (q1^2 + q3^2, q2^2 + q3^2, q1^2 + q2^2, _)
		__m128 vTemp1 = _mm_add_ps(_mm_shuffle_ps(vQ2, vQ2, V_SHUFFLE_XYZW(0, 1, 0, 0)), vTemp0);

		// (1 - 2 * q1^2 - 2 * q3^2, 1 - 2 * q2^2 - 2 * q3^2, 1 - 2 *q1^2 - 2 * q2^2, _)
		const __m128 vDiag = _mm_fmadd_ps(vTemp1, vMin2, vOne);

		// (2q1q4, 2q2q4, 2q1q3, 2q3q4)
		__m128 vTemp3 = _mm_mul_ps(vQ, _mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(3, 3, 0, 2)));
		vTemp3 = _mm_mul_ps(vTemp3, v2);
		// (2q1q2, 2q2q3, 2q3q4, 2q1q3)
		__m128 vTemp2 = _mm_mul_ps(vQ, _mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(1, 2, 3, 0)));
		vTemp2 = _mm_mul_ps(vTemp2, v2);
		vTemp2 = _mm_insert_ps(vTemp2, vTemp3, 0xb0);

		// (2q1q2, 2q1q3, 2q2q3, 2q1q2)
		__m128 vTemp4 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(0, 3, 1, 0));
		// (2q3q4, 2q2q4, 2q1q4, 2q3q4)
		__m128 vTemp5 = _mm_shuffle_ps(vTemp3, vTemp3, V_SHUFFLE_XYZW(3, 1, 0, 3));

		// (2q1q2 + 2q3q4, 2q1q3 - 2q2q4, 2q2q3 + 2q1q4, 2q1q2 - 2q3q4)
		vTemp0 = _mm_addsub_ps(vTemp4, minus(vTemp5));

		// (2q1q3, 2q2q3, _, _)
		__m128 vTemp6 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(3, 1, 0, 0));
		// (2q2q4, 2q1q4, _, _)
		__m128 vTemp7 = _mm_shuffle_ps(vTemp3, vTemp3, V_SHUFFLE_XYZW(1, 0, 0, 0));

		// (2q1q3 + 2q2q4, 2q2q3 - 2q1q4, _, _)
		vTemp1 = _mm_addsub_ps(vTemp6, minus(vTemp7));

		v_float4x4 vR;
		vR.vRow[0] = _mm_insert_ps(_mm_shuffle_ps(vTemp0, vTemp0, V_SHUFFLE_XYZW(0, 0, 1, 0)),
			vDiag, 0x48);
		vR.vRow[1] = _mm_insert_ps(_mm_shuffle_ps(vTemp0, vTemp0, V_SHUFFLE_XYZW(3, 0, 2, 0)),
			vDiag, 0x18);
		vR.vRow[2] = _mm_insert_ps(vTemp1, vDiag, 0xa8);
		vR.vRow[3] = _mm_insert_ps(vOne, vOne, 0xf7);

		return vR;
	}

	// TODO complete
	//		__forceinline __m128 __vectorcall quatFromRotationMatrix(const v_float4x4 vM) noexcept
	//		{
	//			// q4 = 0.5f * (sqrt(trace(M) + 1)
	//			// q1 = (M_23 - M_32) / (4 * q_4)
	//			// q2 = (M_31 - M_13) / (4 * q_4)
	//			// q3 = (M_12 - M_21) / (4 * q_4)
	//
	//			const __m128 vOne = _mm_set1_ps(1.0f);
	//			const __m128 vOneDiv2 = _mm_set1_ps(0.5f);
	//			const __m128 vFour = _mm_set1_ps(4.0f);
	//
	//			__m128 vTrace = _mm_add_ps(vM.vRow[0], _mm_shuffle_ps(vM.vRow[1], vM.vRow[1], V_SHUFFLE_XYZW(1, 1, 1, 1)));
	//			vTrace = _mm_add_ps(vTrace, _mm_shuffle_ps(vM.vRow[2], vM.vRow[2], V_SHUFFLE_XYZW(2, 2, 2, 2)));
	//
	//			__m128 vQ4 = _mm_add_ps(vTrace, vOne);
	//			vQ4 = _mm_mul_ps(vOneDiv2, _mm_sqrt_ps(vQ4));
	//
	//#ifdef _DEBUG
	//			int mask = _mm_movemask_ps(_mm_cmpgt_ps(_mm_set1_ps(FLT_EPSILON), vQ4));
	//			Assert(mask & 0x1 == 0, "Divide by zero");
	//#endif // _DEBUG
	//
	//
	//
	//		}

	__forceinline v_float4x4 __vectorcall translate(float x, float y, float z) noexcept
	{
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vOne = _mm_set_ps1(1.0f);
		const __m128 vT = _mm_setr_ps(x, y, z, 0.0f);

		return v_float4x4(_mm_blend_ps(vZero, vOne, _MM_BLEND_XYZW(1, 0, 0, 0)),
			_mm_blend_ps(vZero, vOne, _MM_BLEND_XYZW(0, 1, 0, 0)),
			_mm_blend_ps(vZero, vOne, _MM_BLEND_XYZW(0, 0, 1, 0)),
			_mm_blend_ps(vT, vOne, _MM_BLEND_XYZW(0, 0, 0, 1)));
	}

	__forceinline v_float4x4 __vectorcall translate(float4a t) noexcept
	{
		const __m128 vZero = _mm_setzero_ps();
		const __m128 vOne = _mm_set_ps1(1.0f);
		const __m128 vT = _mm_load_ps(reinterpret_cast<float*>(&t));

		return v_float4x4(_mm_blend_ps(vZero, vOne, _MM_BLEND_XYZW(1, 0, 0, 0)),
			_mm_blend_ps(vZero, vOne, _MM_BLEND_XYZW(0, 1, 0, 0)),
			_mm_blend_ps(vZero, vOne, _MM_BLEND_XYZW(0, 0, 1, 0)),
			_mm_blend_ps(vT, vOne, _MM_BLEND_XYZW(0, 0, 0, 1)));
	}

	__forceinline v_float4x4 __vectorcall affineTransformation(float4a s, float4a q, float4a t) noexcept
	{
		v_float4x4 vS = scale(s);
		v_float4x4 vR = rotationMatrixFromQuat(_mm_load_ps(reinterpret_cast<float*>(&q)));

		v_float4x4 vM = mul(vS, vR);
		vM.vRow[3] = _mm_load_ps(reinterpret_cast<float*>(&t));

		return vM;
	}

	__forceinline v_float4x4 __vectorcall affineTransformation(const __m128 vS, const __m128 vQ, const __m128 vT) noexcept
	{
		v_float4x4 vScaleM = scale(vS);
		v_float4x4 vRotM = rotationMatrixFromQuat(vQ);

		v_float4x4 vM = mul(vScaleM, vRotM);
		vM.vRow[3] = vT;
		_mm_insert_ps(vM.vRow[3], _mm_set1_ps(1.0f), 0x30);	// set M[3][3] element to 1.0f

		return vM;
	}

	// Decomposes an affine transformation matrix. Assumes transformations were
	// multiplied in the SRT order. S, R and T stand for scaling, rotation and translation respectively
	// (does not correctly handle negative scales right now)
	//__forceinline v_float4x4 __vectorcall decomposeAffine(const v_float4x4 vM, float4a& s, float4a& q, float4a& t) noexcept
	//{
	//	_mm_store_ps(reinterpret_cast<float*>(&t), vM.vRow[3]);

	//	const __m128 vSx2 = _mm_dp_ps(vM.vRow[0], vM.vRow[0], 0xff);
	//	const __m128 vSx = _mm_sqrt_ps(vSx2);
	//	const __m128 vRotRow0 = _mm_div_ps(vM.vRow[0], vSx);

	//	const __m128 vSy2 = _mm_dp_ps(vM.vRow[1], vM.vRow[1], 0xff);
	//	const __m128 vSy = _mm_sqrt_ps(vSy2);
	//	const __m128 vRotRow1 = _mm_div_ps(vM.vRow[1], vSy);

	//	const __m128 vSz2 = _mm_dp_ps(vM.vRow[2], vM.vRow[2], 0xff);
	//	const __m128 vSz = _mm_sqrt_ps(vSz2);
	//	const __m128 vRotRow2 = _mm_div_ps(vM.vRow[2], vSz);

	//	v_float4x4 vR(vRotRow0, vRotRow1, vRotRow2, _mm_setzero_ps());
	//	const __m128 vQ = quatFromRotationMatrix(vR);
	//	_mm_store_ps(reinterpret_cast<float*>(&q), vQ);

	//	float4a temp;
	//	_mm_store_ps(reinterpret_cast<float*>(&temp), vSx);
	//	s.x = temp.x;
	//	_mm_store_ps(reinterpret_cast<float*>(&temp), vSy);
	//	s.y = temp.x;
	//	_mm_store_ps(reinterpret_cast<float*>(&temp), vSz);
	//	s.z = temp.x;
	//}

	__forceinline v_float4x4 __vectorcall lookAtLH(float4a cameraPos, float4a focus, float4a up) noexcept
	{
		v_float4x4 vM = identity();

		// builds a coordiante system uvw, where w is aligned with the camera direction
		__m128 vCamPos = _mm_load_ps(reinterpret_cast<float*>(&cameraPos));
		__m128 vFocus = _mm_load_ps(reinterpret_cast<float*>(&focus));
		__m128 vUp = _mm_load_ps(reinterpret_cast<float*>(&up));

		__m128 vW = _mm_sub_ps(vFocus, vCamPos);
		vW = normalize(vW);
		__m128 vU = cross(vUp, vW);
		vU = normalize(vU);		
		__m128 vV = cross(vW, vU);	// no need to normalize U

		vM.vRow[0] = vU;
		vM.vRow[1] = vV;
		vM.vRow[2] = vW;
		vM = transpose(vM);

		__m128 vTemp = _mm_mul_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(0, 0, 0, 0)), vM.vRow[0]);
		vTemp = _mm_fmadd_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(1, 1, 1, 0)), vM.vRow[1], vTemp);
		vTemp = _mm_fmadd_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(2, 2, 2, 0)), vM.vRow[2], vTemp);

		vM.vRow[3] = _mm_insert_ps(minus(vTemp), vM.vRow[3], 0xf0);

		return vM;
	}

	__forceinline v_float4x4 __vectorcall perspective(float aspectRatio, float vFOV, float nearZ, float farZ) noexcept
	{
		v_float4x4 P;

		float t = 1.0f / tanf(0.5f * vFOV);
		float fSubn = farZ / (farZ - nearZ);

		__m128 vTemp = _mm_setr_ps(t / aspectRatio, t, fSubn, -nearZ * fSubn);
		const __m128 vOne = _mm_set1_ps(1.0f);
		P.vRow[0] = _mm_insert_ps(vTemp, vTemp, 0xe);
		P.vRow[1] = _mm_insert_ps(vTemp, vTemp, 0xd);
		P.vRow[2] = _mm_insert_ps(vTemp, vOne, 0x33);
		P.vRow[3] = _mm_insert_ps(vTemp, vTemp, 0xeb);

		return P;
	}

	__forceinline v_float4x4 perspectiveReverseZ(float aspectRatio, float vFOV, float nearZ) noexcept
	{
		v_float4x4 P;

		float t = 1.0f / tanf(0.5f * vFOV);

		__m128 vTemp = _mm_setr_ps(t / aspectRatio, t, 0.0f, nearZ);
		const __m128 vOne = _mm_set1_ps(1.0f);
		P.vRow[0] = _mm_insert_ps(vTemp, vTemp, 0xe);
		P.vRow[1] = _mm_insert_ps(vTemp, vTemp, 0xd);
		P.vRow[2] = _mm_insert_ps(vTemp, vOne, 0x33);
		P.vRow[3] = _mm_insert_ps(vTemp, vTemp, 0xeb);

		return P;
	}

	__forceinline bool __vectorcall equal(v_float4x4 vM1, v_float4x4 vM2) noexcept
	{
		const __m256 vEps = _mm256_set1_ps(FLT_EPSILON);

		const __m256 vTemp1 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM1.vRow[0]), vM1.vRow[1], 0x1);
		const __m256 vTemp2 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM1.vRow[2]), vM1.vRow[3], 0x1);
		const __m256 vTemp3 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM2.vRow[0]), vM2.vRow[1], 0x1);
		const __m256 vTemp4 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM2.vRow[2]), vM2.vRow[3], 0x1);

		const __m256 vRes1 = _mm256_cmp_ps(vEps, abs(_mm256_sub_ps(vTemp1, vTemp3)), _CMP_GE_OQ);
		const __m256 vRes2 = _mm256_cmp_ps(vEps, abs(_mm256_sub_ps(vTemp2, vTemp4)), _CMP_GE_OQ);

		int r1 = _mm256_movemask_ps(vRes1);
		int r2 = _mm256_movemask_ps(vRes2);

		return (r1 & r2) == 0xff;
	}

	__forceinline v_float4x4 __vectorcall load(float4x4a M) noexcept
	{
		v_float4x4 vM;

		vM.vRow[0] = _mm_load_ps(reinterpret_cast<float*>(&M.m[0]));
		vM.vRow[1] = _mm_load_ps(reinterpret_cast<float*>(&M.m[1]));
		vM.vRow[2] = _mm_load_ps(reinterpret_cast<float*>(&M.m[2]));
		vM.vRow[3] = _mm_load_ps(reinterpret_cast<float*>(&M.m[3]));

		return vM;
	}

	__forceinline v_float4x4 __vectorcall load(float4x3 M) noexcept
	{
		v_float4x4 vM;
		float4x4a temp(M);

		vM.vRow[0] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[0]));
		vM.vRow[1] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[1]));
		vM.vRow[2] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[2]));
		vM.vRow[3] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[3]));

		return vM;
	}

	__forceinline float4x4a __vectorcall store(v_float4x4 M) noexcept
	{
		float4x4a m;

		_mm_store_ps(reinterpret_cast<float*>(&m.m[0]), M.vRow[0]);
		_mm_store_ps(reinterpret_cast<float*>(&m.m[1]), M.vRow[1]);
		_mm_store_ps(reinterpret_cast<float*>(&m.m[2]), M.vRow[2]);
		_mm_store_ps(reinterpret_cast<float*>(&m.m[3]), M.vRow[3]);

		return m;
	}
}