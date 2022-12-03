#pragma once

#include "Common.h"
#include "../Core/Vertex.h"
#include "../Utility/Span.h"
#include "../RenderPass/Common/HLSLCompat.h"

namespace ZetaRay::Math
{
	bool ComputeMeshTangentVectors(Util::Span<Core::Vertex> vertices, Util::Span<INDEX_TYPE> indices,
		bool rhsIndices = false) noexcept;

	// Returns barrycentric-coordinates (u, v, w) of point p relative to triangle v0v1v2 (ordered clockwise)
	// such that p = V0 + v(V1 - V0) + w(V2 - V0) or alternatively,
	//           p = uV0 + vV1 + wV2
	__forceinline __m128 __vectorcall computeBarryCoords(const __m128 v0, const __m128 v1, const __m128 v2, const __m128 p) noexcept
	{
		const __m128 v1Minv0 = _mm_sub_ps(v1, v0);	// s
		const __m128 v2Minv0 = _mm_sub_ps(v2, v0);	// t
		const __m128 pMinv0 = _mm_sub_ps(p, v0);	// q
		const __m128 vOne = _mm_set1_ps(1.0f);

		//		| q.s  t.s|				| s.s  q.s|
		//		| q.t  t.t|				| s.t  q.t|
		// v = -------------       w = -------------
		//		| s.s  t.s|				| s.s  t.t|
		//		| s.t  t.t|				| s.t  t.t|

		// (s_x, s_x, t_x, _)
		__m128 vTemp0 = _mm_shuffle_ps(v1Minv0, v2Minv0, V_SHUFFLE_XYZW(0, 0, 0, 0));
		// (s_y, s_y, t_y, _)
		__m128 vTemp2 = _mm_shuffle_ps(v1Minv0, v2Minv0, V_SHUFFLE_XYZW(1, 1, 1, 1));
		// (s_x, t_x, t_x, _)
		__m128 vTemp1 = _mm_shuffle_ps(vTemp0, vTemp0, V_SHUFFLE_XYZW(0, 2, 2, 0));
		__m128 vDots = _mm_mul_ps(vTemp0, vTemp1);

		// (s_y, t_y, t_y, _)
		__m128 vTemp3 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(0, 2, 2, 0));
		__m128 vTemp4 = _mm_shuffle_ps(v1Minv0, v2Minv0, V_SHUFFLE_XYZW(2, 2, 2, 2));
		vDots = _mm_fmadd_ps(vTemp2, vTemp3, vDots);

		// (s_z, s_z, t_z, _)
		__m128 vTemp5 = _mm_shuffle_ps(vTemp4, vTemp4, V_SHUFFLE_XYZW(0, 2, 2, 0));
		// vDots = (s.s, s.t, t.t, _)
		vDots = _mm_fmadd_ps(vTemp4, vTemp5, vDots);

		vTemp0 = _mm_shuffle_ps(pMinv0, pMinv0, V_SHUFFLE_XYZW(0, 0, 1, 1));
		vTemp1 = _mm_shuffle_ps(pMinv0, pMinv0, V_SHUFFLE_XYZW(2, 2, 2, 2));
		// (qx, qx, qy, qy, qz, qz, _, _)
		__m256 vTemp6 = _mm256_insertf128_ps(_mm256_castps128_ps256(vTemp0), vTemp1, 0x1);

		vTemp2 = _mm_unpacklo_ps(v1Minv0, v2Minv0);
		vTemp3 = _mm_unpackhi_ps(v1Minv0, v2Minv0);
		// (sx, tx, sy, ty, sz, tz, _, _)
		__m256 vTemp7 = _mm256_insertf128_ps(_mm256_castps128_ps256(vTemp2), vTemp3, 0x1);

		// (qx * sx, qx * tx, qy * sy, qy * ty, qz * sz, qz * tz, _, _)
		vTemp6 = _mm256_mul_ps(vTemp6, vTemp7);
		vTemp4 = _mm256_extractf128_ps(vTemp6, 0);
		vTemp5 = _mm256_extractf128_ps(vTemp6, 1);

		vTemp4 = _mm_add_ps(vTemp4, _mm_shuffle_ps(vTemp4, vTemp4, V_SHUFFLE_XYZW(2, 3, 2, 3)));
		// (q.s, q.t, _, _)
		const __m128 vRhs = _mm_add_ps(vTemp4, vTemp5);

		// (q.t, q.s, s.t, _)
		vTemp2 = _mm_shuffle_ps(vRhs, vDots, V_SHUFFLE_XYZW(1, 0, 1, 0));
		// (s.t, s.t, s.t, _)
		vTemp3 = _mm_shuffle_ps(vDots, vDots, V_SHUFFLE_XYZW(1, 1, 1, 0));
		vTemp4 = _mm_mul_ps(vTemp2, vTemp3);

		// (q.s, q.t, s.s, _)
		vTemp0 = _mm_shuffle_ps(vRhs, vDots, V_SHUFFLE_XYZW(0, 1, 0, 0));
		// (t.t, s.s, t.t, _)
		vTemp1 = _mm_shuffle_ps(vDots, vDots, V_SHUFFLE_XYZW(2, 0, 2, 0));
		vTemp4 = _mm_fmsub_ps(vTemp0, vTemp1, vTemp4);

		const __m128 vDetRcp = _mm_rcp_ps(_mm_shuffle_ps(vTemp4, vTemp4, V_SHUFFLE_XYZW(2, 2, 2, 2)));
		vTemp4 = _mm_mul_ps(vTemp4, vDetRcp);

		vTemp2 = _mm_add_ps(vTemp4, _mm_shuffle_ps(vTemp4, vTemp4, V_SHUFFLE_XYZW(1, 1, 1, 1)));
		vTemp2 = _mm_sub_ps(vOne, vTemp2);

		// the remaining barry coordinate is equal to 1 - v - w
		vTemp4 = _mm_insert_ps(_mm_shuffle_ps(vTemp4, vTemp4, V_SHUFFLE_XYZW(0, 0, 1, 0)), vTemp2, 0x0);

		return vTemp4;
	}

	// Finds the axis with the largest span
	//static int FindAxisWithMaximumExtent(const DirectX::XMFLOAT3& vec) noexcept
	//{
	//	const float EPS = 1e-5;
	//	Check(vec.x + vec.y + vec.z > EPS, "Degenerate input vector", SEVERITY::FATAL);

	//	bool xGTy = abs(vec.x) > abs(vec.y);
	//	bool xGTz = abs(vec.x) > abs(vec.z);
	//	bool yGTz = abs(vec.y) > abs(vec.z);

	//	bool maxIsX = xGTy && xGTz;
	//	bool maxIsY = (!xGTy && yGTz);
	//	bool maxIsZ = (!xGTz && !yGTz);

	//	return maxIsX ? 0 : (maxIsY ? 1 : 2);
	//}
}