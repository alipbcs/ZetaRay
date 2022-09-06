#include <Core/ZetaRay.h>
#include <Math/CollisionFuncs.h>
#include <Math/Quaternion.h>
#include <Math/MatrixFuncs.h>
#include <Utility/RNG.h>
#include <doctest-2.4.8/doctest.h>
#include <DirectXMath.h>
#include <DirectXCollision.h>

using namespace ZetaRay;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

TEST_CASE("AABBvsAABB")
{
	int unused;
	RNG rng(reinterpret_cast<uintptr_t>(&unused));

	for (int i = 0; i < 10000; i++)
	{
		float cx = rng.GetUniformFloat() * 1000.0f - rng.GetUniformFloat() * 1000.0f;
		float cy = rng.GetUniformFloat() * 1000.0f - rng.GetUniformFloat() * 1000.0f;
		float cz = rng.GetUniformFloat() * 1000.0f - rng.GetUniformFloat() * 1000.0f;

		float ex = 0.1f + rng.GetUniformFloat() * 1000.0f;
		float ey = 0.1f + rng.GetUniformFloat() * 1000.0f;
		float ez = 0.1f + rng.GetUniformFloat() * 1000.0f;

		DirectX::XMFLOAT3 xc1 = DirectX::XMFLOAT3(cx, cy, cz);
		DirectX::XMFLOAT3 xe1 = DirectX::XMFLOAT3(ex, ey, ez);
		DirectX::BoundingBox xb1(xc1, xe1);
		float3 c1 = float3(cx, cy, cz);
		float3 e1 = float3(ex, ey, ez);
		v_AABB b1(c1, e1);

		cx = rng.GetUniformFloat() * 1000.0f - rng.GetUniformFloat() * 1000.0f;
		cy = rng.GetUniformFloat() * 1000.0f - rng.GetUniformFloat() * 1000.0f;
		cz = rng.GetUniformFloat() * 1000.0f - rng.GetUniformFloat() * 1000.0f;

		ex = 0.1f + rng.GetUniformFloat() * 1000.0f;
		ey = 0.1f + rng.GetUniformFloat() * 1000.0f;
		ez = 0.1f + rng.GetUniformFloat() * 1000.0f;

		DirectX::XMFLOAT3 xc2 = DirectX::XMFLOAT3(cx, cy, cz);
		DirectX::XMFLOAT3 xe2 = DirectX::XMFLOAT3(ex, ey, ez);
		DirectX::BoundingBox xb2(xc2, xe2);
		float3 c2 = float3(cx, cy, cz);
		float3 e2 = float3(ex, ey, ez);
		v_AABB b2(c2, e2);

		auto xr = xb1.Contains(xb2);
		auto r = intersectAABBvsAABB(b1, b2);

		INFO("Center1(", c1.x, ", ", c1.y, ", ", c1.z, ")", "Extents1(", e1.x, ", ", e1.y, ", ", e1.z, ")");
		INFO("Center2(", c2.x, ", ", c2.y, ", ", c2.z, ")", "Extents2(", e2.x, ", ", e2.y, ", ", e2.z, ")");
		INFO("DirectXMath:BoundingBox: ", xr, "ZetaRay: ", (int)r);
		CHECK(xr == (int)r);
	}
}

TEST_CASE("AABBvsFrustum")
{
	float r = 1920.0f / 1080.0f;
	float n = 1.0f;
	float f = 1000.0f;
	DirectX::XMMATRIX P = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, r, n, f);
	DirectX::BoundingFrustum frustum;
	DirectX::BoundingFrustum::CreateFromMatrix(frustum, P);
	ViewFrustum q(DirectX::XM_PIDIV4, r, n, f);
	v_ViewFrustum vf(q);

	RNG rng(reinterpret_cast<uintptr_t>(&vf));
	INFO("Seed: ", reinterpret_cast<uintptr_t>(&vf));

	for (int i = 0; i < 10000; i++)
	{
		float cx = -1000.0f + rng.GetUniformFloat() * 1000.0f;
		float cy = -1000.0f + rng.GetUniformFloat() * 1000.0f;
		float cz = -1000.0f + rng.GetUniformFloat() * 1000.0f;

		float ex = 0.1f + rng.GetUniformFloat() * 1000.0f;
		float ey = 0.1f + rng.GetUniformFloat() * 1000.0f;
		float ez = 0.1f + rng.GetUniformFloat() * 1000.0f;

		DirectX::XMFLOAT3 xc1 = DirectX::XMFLOAT3(cx, cy, cz);
		DirectX::XMFLOAT3 xe1 = DirectX::XMFLOAT3(ex, ey, ez);
		DirectX::BoundingBox xb1(xc1, xe1);

		float3 c1 = float3(cx, cy, cz);
		float3 e1 = float3(ex, ey, ez);
		v_AABB b1(c1, e1);

		auto ct = frustum.Contains(xb1);
		bool xr = (ct == DirectX::INTERSECTS) || (ct == DirectX::CONTAINS);

		auto r = instersectFrustumVsAABB(vf, b1);
		bool zr = (r == COLLISION_TYPE::CONTAINS) || (r == COLLISION_TYPE::INTERSECTS);

		INFO("Center:(", c1.x, ", ", c1.y, ", ", c1.z, ")", "Extents:(", e1.x, ", ", e1.y, ", ", e1.z, ")");
		INFO("DirectXMath result: ", xr, "ZetaRay result: ", zr);
		CHECK(zr == xr);
	}
}

TEST_CASE("QuaternionSlerp")
{
	// Note: setting theta=3.14159f causes test results to be different due to the difference
	// in computation of cos(theta) using CRT's cosf() in rotationQuat() & XMScalarSinCos() in DirectXMath. 
	// See commented lines below

	const int numTests = 10000;
	RNG rng(reinterpret_cast<uintptr_t>(&numTests));
	INFO("Seed: ", reinterpret_cast<uintptr_t>(&numTests));

	for (int i = 0; i < numTests; i++)
	{
		float3 axis1(rng.GetUniformUintBounded(100) - 50.0f, rng.GetUniformUintBounded(100) - 50.0f, rng.GetUniformUintBounded(100) - 50.0f);
		axis1.normalize();
		float theta1 = rng.GetUniformFloat() * TWO_PI;

		float3 axis2(rng.GetUniformUintBounded(100) - 50.0f, rng.GetUniformUintBounded(100) - 50.0f, rng.GetUniformUintBounded(100) - 50.0f);
		axis2.normalize();
		float theta2 = rng.GetUniformFloat() * TWO_PI;

		float t = rng.GetUniformFloat();

		__m128 vQ1 = rotationQuat(axis1, theta1);
		__m128 vQ2 = rotationQuat(axis2, theta2);
		__m128 vQ3 = slerp(vQ1, vQ2, t);
		auto q = store(vQ3);

		auto xaxis1 = DirectX::XMVectorSet(axis1.x, axis1.y, axis1.z, 0.0f);
		//auto Q0 = DirectX::XMQuaternionRotationNormal(xaxis1, theta1);
		auto Q0 = vQ1;
		auto xaxis2 = DirectX::XMVectorSet(axis2.x, axis2.y, axis2.z, 0.0f);
		//auto Q1 = DirectX::XMQuaternionRotationNormal(xaxis2, theta2);
		auto Q1 = vQ2;
		auto xq3 = DirectX::XMQuaternionSlerp(Q0, Q1, t);
		DirectX::XMFLOAT4 ff;
		DirectX::XMStoreFloat4(&ff, xq3);

		constexpr float EPS = 1e-2f;

		auto q1 = store(vQ1);
		auto q2 = store(vQ2);
		auto q3 = store(vQ3);

		DirectX::XMFLOAT4 xQ0;
		DirectX::XMStoreFloat4(&xQ0, Q0);
		DirectX::XMFLOAT4 xQ1;
		DirectX::XMStoreFloat4(&xQ1, Q1);

		INFO("axis1:(", axis1.x, ", ", axis1.y, ", ", axis1.z, ")");
		INFO("theta1:", theta1);
		INFO("axis2:(", axis2.x, ", ", axis2.y, ", ", axis2.z, ")");
		INFO("theta2:", theta2);
		INFO("zq1:(", q1.x, ", ", q1.y, ", ", q1.z, ", ", q1.w);
		INFO("zq2:(", q2.x, ", ", q2.y, ", ", q2.z, ", ", q2.w);
		INFO("xq1:(", xQ0.x, ", ", xQ0.y, ", ", xQ0.z, ", ", xQ0.w);
		INFO("xq2:(", xQ1.x, ", ", xQ1.y, ", ", xQ1.z, ", ", xQ1.w);
		INFO("t:", t);
		INFO("ZetaRay:(", q3.x, ", ", q3.y, ", ", q3.z, ", ", q3.w);
		INFO("DirectXMath:(", ff.x, ", ", ff.y, ", ", ff.z, ", ", ff.w);

		bool isEqual = fabsf(q.x - ff.x) <= EPS &&
			fabsf(q.y - ff.y) <= EPS &&
			fabsf(q.z - ff.z) <= EPS &&
			fabsf(q.w - ff.w) <= EPS;

		CHECK(isEqual == true);
	}
}

TEST_CASE("RayVsAABB")
{
	int unused;
	RNG rng(reinterpret_cast<uintptr_t>(&unused));
	INFO("Seed: ", reinterpret_cast<uintptr_t>(&unused));

	auto getc = [&rng]() {return rng.GetUniformFloat() * 100.0f - rng.GetUniformFloat() * 100.0f; };
	auto gete = [&rng]() {return 1.0f + rng.GetUniformFloat() * 1000.0f; };
	auto geto = [&rng]() {return rng.GetUniformFloat() * 50.0f - rng.GetUniformFloat() * 50.0f; };
	auto getd = [&rng]()
	{
		float f = rng.GetUniformFloat();
		bool z = f > 0.1f;
		float3 res(0.01f + rng.GetUniformFloat(), 0.01f + rng.GetUniformFloat(), 0.01f + rng.GetUniformFloat());

		if (z)
		{
			uint32_t c = rng.GetUniformUint() % 3;

			if (c == 0)
				res.x = 0.0f;
			else if (c == 1)
				res.y = 0.0f;
			else
				res.z = 0.0f;
		}

		return res;
	};

	const int N = 100000;

	for (int i = 0; i < N; i++)
	{
		float3 Center(getc(), getc(), getc());
		float3 Extents(gete(), gete(), gete());
		float3 Origin(geto(), geto(), geto());
		float3 Dir = getd();
		Dir.normalize();

		DirectX::BoundingBox xb1(DirectX::XMFLOAT3(Center.x, Center.y, Center.z), DirectX::XMFLOAT3(Extents.x, Extents.y, Extents.z));
		v_AABB b1(Center, Extents);

		float t;
		auto resDx = xb1.Intersects(DirectX::XMVectorSet(Origin.x, Origin.y, Origin.z, 1.0f),
			DirectX::XMVectorSet(Dir.x, Dir.y, Dir.z, 0.0f), t);

		v_Ray vRay(Origin, Dir);
		auto resZeta = intersectRayVsAABB(vRay, b1, t);

		INFO("Center:(", Center.x, ", ", Center.y, ", ", Center.z, ")", "Extents:(", Extents.x, ", ", Extents.y, ", ", Extents.z, ")");
		INFO("Origin:(", Origin.x, ", ", Origin.y, ", ", Origin.z, ")", "Dir:(", Dir.x, ", ", Dir.y, ", ", Dir.z, ")");
		INFO("DirecXMath: ", resDx, ", ZetaRay: ", resZeta);

		CHECK(resDx == resZeta);
	}
}

TEST_CASE("MergingAABBs")
{
	v_AABB vBox1;
	vBox1.vCenter = _mm_setr_ps(-100.0f, -50.0f, -85.0f, 0.0f);
	vBox1.vExtents = _mm_setr_ps(10.0f, 5.0f, 14.0f, 0.0f);

	v_AABB vBox2;

	v_AABB vMergedBox = compueUnionAABB(vBox1, vBox2);
	AABB box = store(vMergedBox);
	AABB box2 = store(vBox1);

	INFO("merging any AAABB with a default-initalized should result in the former AABB");
	bool centerEq = fabsf(box.Center.x - box2.Center.x) < 1e-6 &&
		fabsf(box.Center.y - box2.Center.y) < 1e-6 &&
		fabsf(box.Center.z - box2.Center.z) < 1e-6;
	bool extEq = fabsf(box.Extents.x - box2.Extents.x) < 1e-6 &&
		fabsf(box.Extents.y - box2.Extents.y) < 1e-6 &&
		fabsf(box.Extents.z - box2.Extents.z) < 1e-6;

	CHECK(centerEq == true);
	CHECK(extEq == true);
}

TEST_CASE("LookAt")
{
	float4a camPos(-10.0f, 5.0f, -3.0f, 1.0f);
	float4a focus(0.0f, 0.0f, 0.0f, 1.0f);
	float4a up(0.0f, 1.0f, 0.0f, 0.0f);

	auto viewMat = lookAtLH(camPos, focus, up);
	auto resZ = store(viewMat);

	DirectX::XMVECTOR eye = DirectX::XMVectorSet(camPos.x, camPos.y, camPos.z, camPos.w);
	DirectX::XMVECTOR f = DirectX::XMVectorSet(focus.x, focus.y, focus.z, focus.w);
	DirectX::XMVECTOR u = DirectX::XMVectorSet(up.x, up.y, up.z, up.w);

	auto viewX = DirectX::XMMatrixLookAtLH(eye, f, u);

	DirectX::XMFLOAT4X4 res;
	DirectX::XMStoreFloat4x4(&res, viewX);

	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			CHECK(res.m[i][j] - reinterpret_cast<float*>(&resZ.m[i])[j] <= FLT_EPSILON);
		}
	}
}

TEST_CASE("PerspectiveMat")
{
	int w = 991;
	int h = 561;
	float r = (float)w / h;
	float nearZ = 0.1f;
	float farZ = 1000.0f;
	float fov = Math::DegreeToRadians(85.0f);

	v_float4x4 vP = perspective(r, fov, nearZ, farZ);
	auto resZ = store(vP);

	auto projX = DirectX::XMMatrixPerspectiveFovLH(fov, r, nearZ, farZ);

	DirectX::XMFLOAT4X4 res;
	DirectX::XMStoreFloat4x4(&res, projX);

	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			CHECK(res.m[i][j] - reinterpret_cast<float*>(&resZ.m[i])[j] <= FLT_EPSILON);
		}
	}
}

TEST_CASE("Quaternion")
{
	__m128 vQs[4];
	
	vQs[0] = _mm_setr_ps(-0.5614617466926575f,
		-0.4267434775829315f,
		0.3989279866218567f,
		0.586094856262207f);

	vQs[1] = _mm_setr_ps(-0.703005313873291f,
		-0.1396184116601944f,
		0.13859958946704865f,
		0.6834328770637512f);

	vQs[2] = _mm_setr_ps(-0.6417776942253113f,
		-0.3504522740840912f,
		0.3484876751899719f,
		0.5863965749740601f);

	vQs[3] = _mm_setr_ps(-0.7071068286895752f,
		0.0f,
		0.0f,
		0.7071067094802856f);

	for (int i = 0; i < 4; i++)
	{
		v_float4x4 vR = rotationMatrixFromQuat(vQs[i]);
		auto resZ = store(vR);

		auto quatX = DirectX::XMMatrixRotationQuaternion(vQs[i]);

		DirectX::XMFLOAT4X4 res;
		DirectX::XMStoreFloat4x4(&res, quatX);

		for (int i = 0; i < 4; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				CHECK(res.m[i][j] - reinterpret_cast<float*>(&resZ.m[i])[j] <= FLT_EPSILON);
			}
		}

		/*
		DirectX::XMVECTOR axis;
		float angle;
		DirectX::XMQuaternionToAxisAngle(&axis, &angle, vQs[i]);

		auto qq = quatToAxisAngle(vQs[i]);

		for (int i = 0; i < 4; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				CHECK(res.m[i][j] - reinterpret_cast<float*>(&resZ.m[i])[j] <= FLT_EPSILON);
			}
		}
		*/
	}
}

/*
TEST_CASE("PlaneTransformation")
{
	auto r0 = float4(-0.979171753f, 0.0f, 0.2030335f, 0.0f);
	auto r1 = float4(-0.034616f, 0.98535f, -0.16694f, 0.0f);
	auto r2 = float4(-0.20f, -0.17049f, -0.96483f, 0.0f);
	auto r3 = float4(-1.98634f, 8.5402f, 0.14189f, 1.0f);

	float4x4a(r0, r1, r2, r3);



	DirectX::XMPlaneTransform()
}
*/