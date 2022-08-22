#pragma once

#include "CollisionTypes.h"
#include "VectorFuncs.h"
#include "MatrixFuncs.h"

namespace ZetaRay
{
	namespace Math
	{
		//--------------------------------------------------------------------------------------
		// Functions
		//--------------------------------------------------------------------------------------

		__forceinline __m128 __vectorcall distFromPlane(const __m128 point, const __m128 plane) noexcept
		{
			// plane: n.(p - p0) = 0
			// dist(p, p0) = proj_n(p - p0) = n.(p - p0)
			return abs(_mm_dp_ps(point, plane, 0xf));
		}

		// Computes AABB that encloses the given mesh. Assumes vtxStride - (posOffset + sizeof(float3)) >= sizeof(float)
		__forceinline v_AABB __vectorcall compueMeshAABB(void* data, uint32_t posOffset, uint32_t vtxStride, uint32_t numVertices) noexcept
		{
			uintptr_t dataPtr = (uintptr_t)data + posOffset;
			float3* currPos = reinterpret_cast<float3*>(dataPtr);

			__m128 vPos = _mm_loadu_ps(reinterpret_cast<float*>(currPos));
			__m128 vMin = vPos;
			__m128 vMax = vPos;

			for (int i = 1; i < (int)numVertices; i++)
			{
				currPos = reinterpret_cast<float3*>(dataPtr);

				vPos = _mm_loadu_ps(reinterpret_cast<float*>(currPos));
				vMin = _mm_min_ps(vPos, vMin);
				vMax = _mm_max_ps(vPos, vMax);

				dataPtr += vtxStride;
			}

			v_AABB vBox;

			const __m128 vOneDivTwo = _mm_set1_ps(0.5f);
			vBox.vCenter = _mm_mul_ps(_mm_add_ps(vMax, vMin), vOneDivTwo);
			vBox.vExtents = _mm_mul_ps(_mm_sub_ps(vMax, vMin), vOneDivTwo);

			return vBox;
		}

		// Returns the union of two AABBs
		__forceinline v_AABB __vectorcall compueUnionAABB(const v_AABB vBox1, const v_AABB vBox2) noexcept
		{
			__m128 vMin1 = _mm_sub_ps(vBox1.vCenter, vBox1.vExtents);
			__m128 vMax1 = _mm_add_ps(vBox1.vCenter, vBox1.vExtents);

			__m128 vMin2 = _mm_sub_ps(vBox2.vCenter, vBox2.vExtents);
			__m128 vMax2 = _mm_add_ps(vBox2.vCenter, vBox2.vExtents);

			__m128 vUnionMin = _mm_min_ps(vMin1, vMin2);
			__m128 vUnionMax = _mm_max_ps(vMax1, vMax2);

			v_AABB vRet;
			const __m128 vOneDiv2 = _mm_set1_ps(0.5f);
			vRet.vCenter = _mm_mul_ps(_mm_add_ps(vUnionMin, vUnionMax), vOneDiv2);
			vRet.vExtents = _mm_mul_ps(_mm_sub_ps(vUnionMax, vUnionMin), vOneDiv2);

			return vRet;
		}

		// Computes surface area of an AABB
		__forceinline float __vectorcall computeAABBSurfaceArea(v_AABB vBox) noexcept
		{
			const __m128 vEight = _mm_set1_ps(8.0f);
			const __m128 vYZX = _mm_shuffle_ps(vBox.vExtents, vBox.vExtents, V_SHUFFLE_XYZW(1, 2, 0, 0));
			const __m128 vZXY = _mm_shuffle_ps(vBox.vExtents, vBox.vExtents, V_SHUFFLE_XYZW(2, 0, 1, 0));

			__m128 res = _mm_mul_ps(vBox.vExtents, vYZX);
			res = _mm_fmadd_ps(vBox.vExtents, vZXY, res);
			res = _mm_fmadd_ps(vYZX, vZXY, res);
			res = _mm_mul_ps(res, vEight);
			res = _mm_shuffle_ps(res, res, V_SHUFFLE_XYZW(0, 0, 0, 0));

			return _mm_cvtss_f32(res);
		}

		// Returns how source "intersects" target (i.e. source contains/intersects/disjoins target). Both
		// must be in the same coordinate system
		__forceinline COLLISION_TYPE __vectorcall intersectAABBvsAABB(const v_AABB source, const v_AABB target) noexcept
		{
			__m128 vMinSource = _mm_sub_ps(source.vCenter, source.vExtents);
			__m128 vMaxSource = _mm_add_ps(source.vCenter, source.vExtents);

			__m128 vMinTarget = _mm_sub_ps(target.vCenter, target.vExtents);
			__m128 vMaxTarget = _mm_add_ps(target.vCenter, target.vExtents);

			// contains
			__m128 vTemp0 = _mm_cmpge_ps(vMinTarget, vMinSource);
			__m128 vTemp1 = _mm_cmpge_ps(vMaxSource, vMaxTarget);

			int contains = _mm_movemask_ps(_mm_and_ps(vTemp0, vTemp1));

			// intersects
			__m128 vTemp2 = _mm_cmpge_ps(vMaxTarget, vMinSource);
			__m128 vTemp3 = _mm_cmpge_ps(vMaxSource, vMinTarget);

			int intersects = _mm_movemask_ps(_mm_and_ps(vTemp2, vTemp3));

			return (contains & 0xf) == 0xf ? COLLISION_TYPE::CONTAINS :
				((intersects & 0xf) == 0xf ? COLLISION_TYPE::INTERSECTS : COLLISION_TYPE::DISJOINT);
		}

		// Returns the AABB that results from the intersection of two AABBs
		__forceinline v_AABB __vectorcall computeOverlapAABB(const v_AABB vBox1, const v_AABB vBox2) noexcept
		{
			v_AABB vO;

			__m128 vMin1 = _mm_sub_ps(vBox1.vCenter, vBox1.vExtents);
			__m128 vMax1 = _mm_add_ps(vBox1.vCenter, vBox1.vExtents);

			__m128 vMin2 = _mm_sub_ps(vBox2.vCenter, vBox2.vExtents);
			__m128 vMax2 = _mm_add_ps(vBox2.vCenter, vBox2.vExtents);

			__m128 vOverlapMin = _mm_min_ps(vMax1, vMax2);
			__m128 vOverlapMax = _mm_max_ps(vMin1, vMin2);

			const __m128 vOneDiv2 = _mm_set1_ps(0.5f);
			vO.vCenter = _mm_mul_ps(_mm_add_ps(vOverlapMin, vOverlapMax), vOneDiv2);
			vO.vExtents = _mm_mul_ps(_mm_sub_ps(vOverlapMax, vOverlapMin), vOneDiv2);

			return vO;
		}

		// Returns whether given AABB and plane intersect. Both must be in the same coordinate system
		__forceinline bool __vectorcall intersectAABBvsPlane(const v_AABB vAABB, const __m128 vPlane) noexcept
		{
			// Seperating-axis theorem, use the plane Normal as the axis
			__m128 vProjLengthAlongAxis = _mm_dp_ps(vAABB.vExtents, abs(vPlane), 0xf);
			__m128 vDistFromPlane = distFromPlane(vAABB.vCenter, vPlane);
			__m128 vIntersects = _mm_cmpge_ps(vProjLengthAlongAxis, vDistFromPlane);

			int r = _mm_movemask_ps(vIntersects);
			return r & 0xf;
		}

		// Returns whether given view-frustum contains or intersects given AABB.
		// Assumes plane normals of frustum are already normalized.
		__forceinline COLLISION_TYPE __vectorcall instersectFrustumVsAABB(const v_ViewFrustum vFrustum, const v_AABB vBox) noexcept
		{
			// Seperating-axis theorem, use the plane Normal as the axis

			__m128 vEx = _mm_shuffle_ps(vBox.vExtents, vBox.vExtents, V_SHUFFLE_XYZW(0, 0, 0, 0));
			__m128 vEy = _mm_shuffle_ps(vBox.vExtents, vBox.vExtents, V_SHUFFLE_XYZW(1, 1, 1, 1));
			__m128 vEz = _mm_shuffle_ps(vBox.vExtents, vBox.vExtents, V_SHUFFLE_XYZW(2, 2, 2, 2));

			// (E.x, E.x, E.x, E.x, E.x, E.x, E.x, E.x)
			const __m256 vEx256 = _mm256_insertf128_ps(_mm256_castps128_ps256(vEx), vEx, 0x1);
			// (E.y, E.y, E.y, E.y, E.y, E.y, E.y, E.y)
			const __m256 vEy256 = _mm256_insertf128_ps(_mm256_castps128_ps256(vEy), vEy, 0x1);
			// (E.z, E.z, E.z, E.z, E.z, E.z, E.z, E.z)
			const __m256 vEz256 = _mm256_insertf128_ps(_mm256_castps128_ps256(vEz), vEz, 0x1);

			// absolute value of the plane normals
			__m256 vN_x_abs = abs(vFrustum.vN_x);
			__m256 vN_y_abs = abs(vFrustum.vN_y);
			__m256 vN_z_abs = abs(vFrustum.vN_z);

			// projection of farthest corner on the axis
			__m256 vLargestProjLengthAlongAxis = _mm256_mul_ps(vEx256, vN_x_abs);
			vLargestProjLengthAlongAxis = _mm256_fmadd_ps(vEy256, vN_y_abs, vLargestProjLengthAlongAxis);
			vLargestProjLengthAlongAxis = _mm256_fmadd_ps(vEz256, vN_z_abs, vLargestProjLengthAlongAxis);

			__m128 vCy = _mm_shuffle_ps(vBox.vCenter, vBox.vCenter, V_SHUFFLE_XYZW(1, 1, 1, 1));
			__m128 vCz = _mm_shuffle_ps(vBox.vCenter, vBox.vCenter, V_SHUFFLE_XYZW(2, 2, 2, 2));

			// (C.x, C.x, C.x, C.x, C.x, C.x, C.x, C.x)
			const __m256 vCx256 = _mm256_broadcastss_ps(vBox.vCenter);
			// (C.y, C.y, C.y, C.y, C.y, C.y, C.y, C.y)
			const __m256 vCy256 = _mm256_insertf128_ps(_mm256_castps128_ps256(vCy), vCy, 0x1);
			// (C.z, C.z, C.z, C.z, C.z, C.z, C.z, C.z)
			const __m256 vCz256 = _mm256_insertf128_ps(_mm256_castps128_ps256(vCz), vCz, 0x1);

			// compute the distance of that AABB center from the plane
			__m256 vCenterDistFromPlane = _mm256_mul_ps(vCx256, vFrustum.vN_x);
			vCenterDistFromPlane = _mm256_fmadd_ps(vCy256, vFrustum.vN_y, vCenterDistFromPlane);
			vCenterDistFromPlane = _mm256_fmadd_ps(vCz256, vFrustum.vN_z, vCenterDistFromPlane);
			vCenterDistFromPlane = _mm256_add_ps(vFrustum.vd, vCenterDistFromPlane);

			// AABB is (at least partially) in the poistive half-space of plane. It may or may not intersect the plane
			__m256 vIntersects1 = _mm256_cmp_ps(vCenterDistFromPlane, _mm256_setzero_ps(), _CMP_GE_OQ);
			// AABB intersects the plane
			__m256 vIntersects2 = _mm256_cmp_ps(vLargestProjLengthAlongAxis, abs(vCenterDistFromPlane), _CMP_GE_OQ);

			int r1 = _mm256_movemask_ps(vIntersects1);
			int r2 = _mm256_movemask_ps(vIntersects2);

			// must be true for all the planes
			bool intersects = ((r1 & 0x3f) | (r2 & 0x3f)) == 0x3f;

			return intersects ? COLLISION_TYPE::INTERSECTS : COLLISION_TYPE::DISJOINT;
		}

		// Returns whether given ray and AABB intersect
		__forceinline bool __vectorcall intersectRayVsAABB(const v_Ray vRay, const v_AABB& vBox, float& t) noexcept
		{
			// An AABB can be described as the intersection of three "slabs", where a slab
			// is the (infinite) region of space between two parallel planes.
			// 
			// A given ray intersects an AABB if and only if some segment of the ray intersects the three
			// slabs of the AABB at the same time.
			//
			// To figure out whether that's the case, the ray is intersected with two planes conistuting each
			// slab and two intersections are used to update "farthest entry" (t0) and "nearest exit" (t1) of all 
			// the resulting ray/slab intersection segments. If at some point t0 becomes greater t1, then there's
			// no intersection.

			const __m128 vDirRcp = _mm_div_ps(_mm_set1_ps(1.0f), vRay.vDir);
			const __m128 vDirIsPos = _mm_cmpge_ps(vRay.vDir, _mm_setzero_ps());

			// for better numerical robustness, tranlate the ray center to origin and do
			// the same for AABB
			const __m128 vCenterTranslatedToOrigin = _mm_sub_ps(vBox.vCenter, vRay.vOrigin);

			// (x_min, y_min, z_min)
			__m128 vMin = _mm_sub_ps(vCenterTranslatedToOrigin, vBox.vExtents);
			// (x_max, y_max, z_max)
			__m128 vMax = _mm_add_ps(vCenterTranslatedToOrigin, vBox.vExtents);

			// if ray and AABB are parallel, then the ray origin must be inside the AABB
			const __m128 vZero = _mm_setzero_ps();
			const __m128 vIsParallel = _mm_cmpge_ps(_mm_set1_ps(FLT_EPSILON), abs(vRay.vDir));
			const __m128 vResParallel = _mm_and_ps(vIsParallel, _mm_or_ps(_mm_cmpge_ps(vZero, vMax), _mm_cmpge_ps(vMin, vZero)));

			// t = x_min - (o_x, o_y, o_z) / d
			// t = x_min - (0, 0, 0) / d
			__m128 vTmin = _mm_mul_ps(vMin, vDirRcp);
			__m128 vTmax = _mm_mul_ps(vMax, vDirRcp);

			// if x/y/z component of the ray direction is negative then, then the nearest hit
			// with corresponding slab is with the x_max/y_max/z_max and vice versa for the
			// positive case
			vTmin = _mm_blendv_ps(vTmax, vTmin, vDirIsPos);
			vTmax = _mm_blendv_ps(vTmin, vTmax, vDirIsPos);

			__m128 vT0 = _mm_shuffle_ps(vTmin, vTmin, V_SHUFFLE_XYZW(0, 0, 0, 0));
			__m128 vT1 = _mm_shuffle_ps(vTmax, vTmax, V_SHUFFLE_XYZW(0, 0, 0, 0));

			// find maximum of vTmin
			vT1 = _mm_min_ps(vT1, _mm_shuffle_ps(vTmax, vTmax, V_SHUFFLE_XYZW(1, 1, 1, 1)));
			vT1 = _mm_min_ps(vT1, _mm_shuffle_ps(vTmax, vTmax, V_SHUFFLE_XYZW(2, 2, 2, 2)));
			// if t1 is less than zero, then there's no intersection
			const __m128 vT1IsNegative = _mm_cmpgt_ps(vZero, vT1);

			// find minimum of vTmin
			vT0 = _mm_max_ps(vT0, _mm_shuffle_ps(vTmin, vTmin, V_SHUFFLE_XYZW(1, 1, 1, 1)));
			vT0 = _mm_max_ps(vT0, _mm_shuffle_ps(vTmin, vTmin, V_SHUFFLE_XYZW(2, 2, 2, 2)));

			const __m128 vResNotParallel = _mm_or_ps(_mm_cmpgt_ps(vT0, vT1), vT1IsNegative);
			__m128 vRes = _mm_or_ps(vResNotParallel, vResParallel);

			// https://stackoverflow.com/questions/5526658/intel-sse-why-does-mm-extract-ps-return-int-instead-of-float
			t = _mm_cvtss_f32(vT0);
			int res = _mm_movemask_ps(vRes);

			return (res & 0x7) == 0;
		}

		// Returns whether given ray and AABB intersect
		// When a given Ray is tested against multiple AABBs, a few values that only depend 
		// on that ray can be precomputed to avoid unneccesary recomputations
		__forceinline bool __vectorcall intersectRayVsAABB(const v_Ray vRay, const __m128 vDirRcp,
			const __m128 vDirIsPos, const __m128 vIsParallel, const v_AABB& vBox, float& t) noexcept
		{
			const __m128 vCenterTranslatedToOrigin = _mm_sub_ps(vBox.vCenter, vRay.vOrigin);

			const __m128 vMin = _mm_sub_ps(vCenterTranslatedToOrigin, vBox.vExtents);
			const __m128 vMax = _mm_add_ps(vCenterTranslatedToOrigin, vBox.vExtents);

			const __m128 vZero = _mm_setzero_ps();
			const __m128 vResParallel = _mm_and_ps(vIsParallel, _mm_or_ps(_mm_cmpge_ps(vZero, vMax), _mm_cmpge_ps(vMin, vZero)));

			__m128 vTmin = _mm_mul_ps(vMin, vDirRcp);
			__m128 vTmax = _mm_mul_ps(vMax, vDirRcp);
			vTmin = _mm_blendv_ps(vTmax, vTmin, vDirIsPos);
			vTmax = _mm_blendv_ps(vTmin, vTmax, vDirIsPos);

			__m128 vT0 = _mm_shuffle_ps(vTmin, vTmin, V_SHUFFLE_XYZW(0, 0, 0, 0));
			__m128 vT1 = _mm_shuffle_ps(vTmax, vTmax, V_SHUFFLE_XYZW(0, 0, 0, 0));

			vT1 = _mm_min_ps(vT1, _mm_shuffle_ps(vTmax, vTmax, V_SHUFFLE_XYZW(1, 1, 1, 1)));
			vT1 = _mm_min_ps(vT1, _mm_shuffle_ps(vTmax, vTmax, V_SHUFFLE_XYZW(2, 2, 2, 2)));
			const __m128 vT1IsNegative = _mm_cmpgt_ps(vZero, vT1);

			vT0 = _mm_max_ps(vT0, _mm_shuffle_ps(vTmin, vTmin, V_SHUFFLE_XYZW(1, 1, 1, 1)));
			vT0 = _mm_max_ps(vT0, _mm_shuffle_ps(vTmin, vTmin, V_SHUFFLE_XYZW(2, 2, 2, 2)));

			const __m128 vResNotParallel = _mm_or_ps(_mm_cmpgt_ps(vT0, vT1), vT1IsNegative);
			__m128 vRes = _mm_or_ps(vResNotParallel, vResParallel);

			// https://stackoverflow.com/questions/5526658/intel-sse-why-does-mm-extract-ps-return-int-instead-of-float
			t = _mm_cvtss_f32(vT0);
			int res = _mm_movemask_ps(vRes);

			return (res & 0x7) == 0;
		}

		// Returns whether given ray and triangle formed by vertices v0v1v2 (clockwise order) intersect
		__forceinline bool __vectorcall intersectRayVsTriangle(const v_Ray vRay, __m128 v0,
			__m128 v1, __m128 v2, float& t) noexcept
		{
			// closer to (0, 0, 0) provides better precision, so translate ray origin to (0, 0, 0)
			v0 = _mm_sub_ps(v0, vRay.vOrigin);
			v1 = _mm_sub_ps(v1, vRay.vOrigin);
			v2 = _mm_sub_ps(v2, vRay.vOrigin);

			const __m128 vp = _mm_sub_ps(v2, v0);		// v2 - v0
			const __m128 vq = _mm_sub_ps(v1, v0);		// v1 - v0
			const __m128 vr = minus(v0);				// ray.Orig - v0
			const __m128 vMinDir = minus(vRay.vDir);

			// solve the linear system of equation using the Cramer's rule:
			//		[v1 - v0, v2 - v0, -d] [v w t]^t = [origin - v0]^t
			//
			// v = (origin - v0).((v2 - v0) * d)
			// w = (origin - v0).(-d * (v2 - v0))
			// t = (origin - v0).((v1 - v0) * (v2 - v0))
			//
			// where v, w are barrycentric coords. of hit position such that:
			//		v0 + v(v1 - v0) + w(v2 - v0) = hit_pos

			__m128 vRow0 = cross(vp, vMinDir);	// (v2 - v0) * ray.Dir
			__m128 vRow1 = cross(vMinDir, vq);	// ray.Dir * (v1 - v0)
			__m128 vRow2 = cross(vq, vp);		// (v1 - v0) * (v2 - v0)

			// compute determinant
			__m128 vDet = _mm_dp_ps(vRow2, vMinDir, 0xff);

			//		0  1  2              0  4  8
			// M =	4  5  6 	-->  M = 1  5  9
			//      8  9  10             2  6  10
			const __m128 vTemp0 = _mm_shuffle_ps(vRow0, vRow1, V_SHUFFLE_XYZW(0, 1, 0, 1));	// 0  1  4  5
			const __m128 vTemp1 = _mm_shuffle_ps(vRow0, vRow1, V_SHUFFLE_XYZW(2, 0, 2, 0));	// 2  _  6  _

			vRow0 = _mm_shuffle_ps(vTemp0, vRow2, V_SHUFFLE_XYZW(0, 2, 0, 0));
			vRow1 = _mm_shuffle_ps(vTemp0, vRow2, V_SHUFFLE_XYZW(1, 3, 1, 0));
			vRow2 = _mm_shuffle_ps(vTemp1, vRow2, V_SHUFFLE_XYZW(0, 2, 2, 0));

			// (v, w, t, _)
			__m128 vRes = _mm_mul_ps(_mm_shuffle_ps(vr, vr, V_SHUFFLE_XYZW(0, 0, 0, 0)), vRow0);
			vRes = _mm_fmadd_ps(_mm_shuffle_ps(vr, vr, V_SHUFFLE_XYZW(1, 1, 1, 0)), vRow1, vRes);
			vRes = _mm_fmadd_ps(_mm_shuffle_ps(vr, vr, V_SHUFFLE_XYZW(2, 2, 2, 0)), vRow2, vRes);

			vRes = _mm_div_ps(vRes, vDet);
			__m128 vRayTriParralel = _mm_cmpge_ps(abs(vDet), _mm_set1_ps(FLT_EPSILON));
			int rayTriParralel = _mm_movemask_ps(vRayTriParralel);

			float4a q = store(vRes);
			t = q.z;

			bool insideTri = (q.x >= 0) && (q.x <= 1) && (q.y >= 0) && (q.y <= 1) && (q.x + q.y <= 1.0);
			bool triBehindRay = (t >= 0);

			// determinant is equal to dot product of triangle normal and (negative) ray 
			// direction. If it's zero, then the ray was parallel to triangle. Furthermore,
			// positive determinat means ray hit the front-face of triangle while negative
			// determinat means ray hit the back-face of it

			return insideTri && triBehindRay && (rayTriParralel & 0xf);
		}

		// Ref: Arvo, James. "Transforming axis-aligned bounding boxes", 1990
		__forceinline v_AABB __vectorcall transform(const v_float4x4 M, const v_AABB& aabb) noexcept
		{
			// transform the center
			v_AABB newAABB;
			newAABB.vCenter = mul(M, aabb.vCenter);

			// extents.w = 0, so tranlation doesn't apply
			const __m128 vX = _mm_shuffle_ps(aabb.vExtents, aabb.vExtents, V_SHUFFLE_XYZW(0, 0, 0, 0));
			newAABB.vExtents = _mm_mul_ps(vX, abs(M.vRow[0]));

			const __m128 vY = _mm_shuffle_ps(aabb.vExtents, aabb.vExtents, V_SHUFFLE_XYZW(1, 1, 1, 1));
			newAABB.vExtents = _mm_fmadd_ps(vY, abs(M.vRow[1]), newAABB.vExtents);

			const __m128 vZ = _mm_shuffle_ps(aabb.vExtents, aabb.vExtents, V_SHUFFLE_XYZW(2, 2, 2, 2));
			newAABB.vExtents = _mm_fmadd_ps(vZ, abs(M.vRow[2]), newAABB.vExtents);

			return newAABB;
		}

		// Transforms given view-frustum with a transformation matrix
		__forceinline v_ViewFrustum __vectorcall transform(const v_float4x4 M, const v_ViewFrustum& vFrustum) noexcept
		{
			// In general, planes need to be transformed with the inverse-tranpose of a given transformation M
			// (due to the normal vector). For view-to-world transformation, we know that it only consists
			// of rotation and translations, therefore:
			//		M = R * T
			//		M = ((R * T)^-1)^T
			//		M = (T^-1 * R^-1)^T
			//		M = (T^-1 * R^T)^T	(rotation matrix is orthogonal, so R^-1 == R^T)
			//		M = R * (T^-1)^T
			// 
			// In summary, inverse-tranpose of M is the same as M except for 4th column being:
			//		[M.row0.Tv, M.row1.Tv, M.row2.Tv, 1]^T
			// 
			// with Tv = (-T.x, -T.y, -T.z)
			//
			const v_float4x4 vMT = transpose(M);
			const __m128 vTinv = minus(M.vRow[3]);

			__m128 v4th = _mm_mul_ps(vMT.vRow[0], _mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(0, 0, 0, 0)));
			v4th = _mm_fmadd_ps(vMT.vRow[1], _mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(1, 1, 1, 1)), v4th);
			v4th = _mm_fmadd_ps(vMT.vRow[2], _mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(2, 2, 2, 2)), v4th);

			v_float4x4 vInvT;
			vInvT.vRow[0] = _mm_insert_ps(M.vRow[0], v4th, 0x30);
			vInvT.vRow[1] = _mm_insert_ps(M.vRow[1], v4th, 0x70);
			vInvT.vRow[2] = _mm_insert_ps(M.vRow[2], v4th, 0xb0);
			vInvT.vRow[3] = _mm_insert_ps(M.vRow[3], M.vRow[3], 0x7);

			// transform each of the 6 frustum planes
			// each plane (n_x, n_y, n_z, d) is transformed such that:
			//		transformed_plane = (n_x, n_y, n_z, d) * M		
			//
			//		where M = R * (T^-1)^T

			v_ViewFrustum vRet;

			vRet.vN_x = _mm256_mul_ps(_mm256_broadcastss_ps(vInvT.vRow[0]), vFrustum.vN_x);
			vRet.vN_x = _mm256_fmadd_ps(_mm256_broadcastss_ps(vInvT.vRow[1]), vFrustum.vN_y, vRet.vN_x);
			vRet.vN_x = _mm256_fmadd_ps(_mm256_broadcastss_ps(vInvT.vRow[2]), vFrustum.vN_z, vRet.vN_x);

			__m256 vRow0RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[0]), 4)));
			__m256 vRow1RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[1]), 4)));
			__m256 vRow2RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[2]), 4)));

			vRet.vN_y = _mm256_mul_ps(vRow0RightShifted, vFrustum.vN_x);
			vRet.vN_y = _mm256_fmadd_ps(vRow1RightShifted, vFrustum.vN_y, vRet.vN_y);
			vRet.vN_y = _mm256_fmadd_ps(vRow2RightShifted, vFrustum.vN_z, vRet.vN_y);

			vRow0RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[0]), 8)));
			vRow1RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[1]), 8)));
			vRow2RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[2]), 8)));

			vRet.vN_z = _mm256_mul_ps(vRow0RightShifted, vFrustum.vN_x);
			vRet.vN_z = _mm256_fmadd_ps(vRow1RightShifted, vFrustum.vN_y, vRet.vN_z);
			vRet.vN_z = _mm256_fmadd_ps(vRow2RightShifted, vFrustum.vN_z, vRet.vN_z);

			vRow0RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[0]), 12)));
			vRow1RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[1]), 12)));
			vRow2RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[2]), 12)));
			__m256 vRow3RightShifted = _mm256_broadcastss_ps(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(vInvT.vRow[3]), 12)));

			vRet.vd = _mm256_mul_ps(_mm256_broadcastss_ps(v4th), vFrustum.vN_x);
			vRet.vd = _mm256_fmadd_ps(vRow1RightShifted, vFrustum.vN_y, vRet.vd);
			vRet.vd = _mm256_fmadd_ps(vRow2RightShifted, vFrustum.vN_z, vRet.vd);
			vRet.vd = _mm256_fmadd_ps(vRow3RightShifted, vFrustum.vd, vRet.vd);

			return vRet;
		}

		__forceinline AABB __vectorcall store(v_AABB vBox) noexcept
		{
			AABB aabb;

			float4 f1;
			_mm_storeu_ps(reinterpret_cast<float*>(&f1), vBox.vCenter);
			aabb.Center = float3(f1.x, f1.y, f1.z);

			float4 f2;
			_mm_storeu_ps(reinterpret_cast<float*>(&f2), vBox.vExtents);
			aabb.Extents = float3(f2.x, f2.y, f2.z);

			return aabb;
		}
	}
}
