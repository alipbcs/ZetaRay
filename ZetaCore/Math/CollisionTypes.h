#pragma once

#include "Vector.h"

namespace ZetaRay::Math
{
	enum class COLLISION_TYPE
	{
		DISJOINT = 0,
		INTERSECTS = 1,
		CONTAINS = 2
	};

	//--------------------------------------------------------------------------------------
	// Axis-Aligned Bounding Box
	//--------------------------------------------------------------------------------------

	struct AABB
	{
		AABB() noexcept = default;
		AABB(const float3& c, const float3& e) noexcept
			: Center(c),
			Extents(e)
		{}

		float3 Center = float3(0.0f, 0.0f, 0.0f);
		float3 Extents = float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	};

	struct v_AABB
	{
		v_AABB() noexcept
		{
			auto c = float3(0.0f, 0.0f, 0.0f);
			auto e = float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			Reset(c, e);
		}

		explicit v_AABB(const AABB& aabb) noexcept
		{
			Reset(aabb);
		}

		v_AABB(float3& c, float3& e) noexcept
		{
			Reset(c, e);
		}

		void __vectorcall Reset(const AABB& aabb) noexcept
		{
			vCenter = _mm_loadu_ps(reinterpret_cast<const float*>(&aabb));
			vExtents = _mm_loadu_ps(reinterpret_cast<const float*>(&aabb) + 3);

			vExtents = _mm_insert_ps(vExtents, vCenter, 0xc8);
			// make sure the 4th element is 1.0f so that tranlation transforms apply
			vCenter = _mm_insert_ps(vCenter, _mm_set1_ps(1.0f), 0x30);
		}

		void __vectorcall Reset(float3& c, float3& e) noexcept
		{
			vCenter = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&c)));
			vCenter = _mm_blend_ps(vCenter, _mm_set1_ps(1.0f), 0x8);
			vCenter = _mm_insert_ps(vCenter, _mm_load_ss(&c.z), 0x20);

			vExtents = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&e)));
			vExtents = _mm_insert_ps(vExtents, _mm_load_ss(&e.z), 0x20);
		}

		ZetaInline void __vectorcall Reset(__m128 vMinPoint, __m128 vMaxPoint) noexcept
		{
			const __m128 vOneDivTwo = _mm_set1_ps(0.5f);
			vCenter = _mm_mul_ps(_mm_add_ps(vMaxPoint, vMinPoint), vOneDivTwo);
			// make sure the 4th element is 1.0f so that tranlation transforms apply
			vCenter = _mm_insert_ps(vCenter, _mm_set1_ps(1.0f), 0x30);

			vExtents = _mm_mul_ps(_mm_sub_ps(vMaxPoint, vMinPoint), vOneDivTwo);
		}

		__m128 vCenter;
		__m128 vExtents;
	};

	//--------------------------------------------------------------------------------------
	// Plane with equation n.(p - p0) = 0, where n is the plane normal and p0 is any point 
	// on its surface
	//--------------------------------------------------------------------------------------

	struct Plane
	{
		Plane() noexcept
			: Normal(0.0f, 0.0f, 0.0f),
			d(0.0f)
		{}

		Plane(const float3& n, float d) noexcept
			: Normal(n),
			d(d)
		{}

		Plane(const float3& n, const float3& p0) noexcept
			: Normal(n),
			d(-(n.x * p0.x + n.y * p0.y + n.z * p0.z))
		{}

		float3 Normal;
		float d;
	};

	//--------------------------------------------------------------------------------------
	// View Frustum
	//--------------------------------------------------------------------------------------

	// In view space, centered at the origin (0, 0, 0), looking down the +z-axis
	struct alignas(16) ViewFrustum
	{
		ViewFrustum() noexcept = default;

		ViewFrustum(float vFOV, float aspectRatio, float nearZ, float farZ) noexcept
		{
			Assert(vFOV > 0.0f, "invalid vertical FOV");
			Assert(nearZ > 0.0f && farZ > 0.0f && farZ > nearZ, "invalid near and far planes");
			Assert(aspectRatio > 0.0f, "invalid aspect ratio");
			const float projWndDist = 1.0f / tanf(vFOV * 0.5f);

			float3 n;
			float d;

			// for all the planes, positive half space overlaps inside of the frustum

			// near plane
			{
				n = float3(0.0f, 0.0f, 1.0f);
				d = -nearZ;
				Near = Plane(n, d);
			}

			// far plane
			{
				n = float3(0.0f, 0.0f, -1.0f);
				d = farZ;
				Far = Plane(n, d);
			}

			float l2Norm = 1.0f / sqrtf(projWndDist * projWndDist + 1.0f);

			// top plane
			{
				n = float3(0.0f, -projWndDist, 1.0f);
				n.y *= l2Norm;
				n.z *= l2Norm;
				d = 0.0f;
				Assert(fabsf(1.0f - (n.y * n.y + n.z * n.z)) < 1e-5f, "normal vector is not normalized");
				Top = Plane(n, d);
			}

			// bottom plane
			{
				n = float3(0.0f, projWndDist, 1.0f);
				n.y *= l2Norm;
				n.z *= l2Norm;
				d = 0.0f;
				Assert(fabsf(1.0f - (n.y * n.y + n.z * n.z)) < 1e-5f, "normal vector is not normalized");
				Bottom = Plane(n, d);
			}

			l2Norm = 1.0f / sqrtf(projWndDist * projWndDist + aspectRatio * aspectRatio);

			// left plane
			{
				n = float3(projWndDist, 0.0f, aspectRatio);
				n.x *= l2Norm;
				n.z *= l2Norm;
				d = 0.0f;
				Assert(fabsf(1.0f - (n.x * n.x + n.z * n.z)) < 1e-5f, "normal vector is not normalized");
				Left = Plane(n, d);
			}

			// right plane
			{
				n = float3(-projWndDist, 0.0f, aspectRatio);
				n.x *= l2Norm;
				n.z *= l2Norm;
				d = 0.0f;
				Assert(fabsf(1.0f - (n.x * n.x + n.z * n.z)) < 1e-5f, "normal vector is not normalized");
				Right = Plane(n, d);
			}
		}

		Plane Left;
		Plane Right;
		Plane Top;
		Plane Bottom;
		Plane Near;
		Plane Far;
	};

	struct v_ViewFrustum
	{
		v_ViewFrustum() noexcept
		{}

		explicit v_ViewFrustum(ViewFrustum& f) noexcept
		{
			alignas(32) float N_x[8];
			alignas(32) float N_y[8];
			alignas(32) float N_z[8];
			alignas(32) float d[8];

			N_x[0] = f.Left.Normal.x;
			N_y[0] = f.Left.Normal.y;
			N_z[0] = f.Left.Normal.z;
			d[0] = f.Left.d;

			N_x[1] = f.Right.Normal.x;
			N_y[1] = f.Right.Normal.y;
			N_z[1] = f.Right.Normal.z;
			d[1] = f.Right.d;

			N_x[2] = f.Top.Normal.x;
			N_y[2] = f.Top.Normal.y;
			N_z[2] = f.Top.Normal.z;
			d[2] = f.Top.d;

			N_x[3] = f.Bottom.Normal.x;
			N_y[3] = f.Bottom.Normal.y;
			N_z[3] = f.Bottom.Normal.z;
			d[3] = f.Bottom.d;

			N_x[4] = f.Near.Normal.x;
			N_y[4] = f.Near.Normal.y;
			N_z[4] = f.Near.Normal.z;
			d[4] = f.Near.d;

			N_x[5] = f.Far.Normal.x;
			N_y[5] = f.Far.Normal.y;
			N_z[5] = f.Far.Normal.z;
			d[5] = f.Far.d;

			vN_x = _mm256_load_ps(N_x);
			vN_y = _mm256_load_ps(N_y);
			vN_z = _mm256_load_ps(N_z);
			vd = _mm256_load_ps(d);
		}

		__m256 vN_x;
		__m256 vN_y;
		__m256 vN_z;
		__m256 vd;
	};

	//--------------------------------------------------------------------------------------
	// Ray
	//--------------------------------------------------------------------------------------

	struct Ray
	{
		constexpr Ray() noexcept
		{}

		constexpr Ray(const float3& o, const float3& d) noexcept
			: Origin(o),
			Dir(d)
		{}

		float3 Origin;
		float3 Dir;
	};

	struct v_Ray
	{
		v_Ray() noexcept
		{}

		explicit v_Ray(Ray& r) noexcept
		{
			vOrigin = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&r.Origin)));
			vOrigin = _mm_insert_ps(vOrigin, _mm_load_ss(&r.Origin.z), 0x20);

			vDir = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&r.Dir)));
			vDir = _mm_insert_ps(vDir, _mm_load_ss(&r.Dir.z), 0x20);
		}

		v_Ray(float3& origin, float3& dir) noexcept
		{
			vOrigin = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&origin)));
			vOrigin = _mm_insert_ps(vOrigin, _mm_load_ss(&origin.z), 0x20);

			vDir = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&dir)));
			vDir = _mm_insert_ps(vDir, _mm_load_ss(&dir.z), 0x20);
		}

		__m128 vOrigin;
		__m128 vDir;
	};
}