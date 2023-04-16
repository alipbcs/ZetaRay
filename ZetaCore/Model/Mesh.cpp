#include "Mesh.h"
#include "../Math/Common.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Surface.h"
#include "../Math/MatrixFuncs.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Model;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// TriangleMesh
//--------------------------------------------------------------------------------------

TriangleMesh::TriangleMesh(Util::Span<Core::Vertex> vertices, 
	size_t vtxBuffStartOffset,
	size_t idxBuffStartOffset,
	uint32_t numIndices, 
	uint64_t matID) noexcept
	: m_numIndices(numIndices),
	m_materialID(matID),
	m_vtxBuffStartOffset(vtxBuffStartOffset),
	m_idxBuffStartOffset(idxBuffStartOffset) 
{
	Assert(vertices.size() > 0 && vertices.size() < UINT_MAX, "Invalid number of vertices.");
	Assert(numIndices > 0, "#indices must be greater than zero.");
	//Assert(sizeof(VertexPosNormalTexTangent) - (offsetof(VertexPosNormalTexTangent, Position) + sizeof(float3)) >= sizeof(float), "");

	m_numVertices = (uint32_t)vertices.size();

	v_AABB vBox = compueMeshAABB(vertices.data(), offsetof(Vertex, Position), sizeof(Vertex), m_numVertices);
	m_AABB = store(vBox);

	//size_t sizeInBytes = sizeof(VertexPosNormalTexTangent) * vertices.size();
	//m_vertexBuffer = App::GetRenderer().GetGpuMemory().GetDefaultHeapBufferAndInit("VertexBuffer", sizeInBytes,
	//	D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false, vertices.begin());

	//sizeInBytes = sizeof(uint32_t) * indices.size();
	//m_indexBuffer = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit("IndexBuffer", sizeInBytes,
	//	D3D12_RESOURCE_STATE_INDEX_BUFFER, false, indices.begin());
}

//--------------------------------------------------------------------------------------
// PrimitiveMesh
//--------------------------------------------------------------------------------------

void PrimitiveMesh::ComputeGrid(Util::Vector<Vertex>& vertices, Vector<uint32_t>& indices,
	float width, float depth, uint32_t m, uint32_t n) noexcept
{
	vertices.clear();
	indices.clear();

	uint32_t vertexCount = m * n;
	uint32_t faceCount = (m - 1) * (n - 1) * 2;

	//
	// Create the vertices.
	//

	float halfWidth = 0.5f * width;
	float halfDepth = 0.5f * depth;

	float dx = width / (n - 1);
	float dz = depth / (m - 1);

	float du = 1.0f / (n - 1);
	float dv = 1.0f / (m - 1);

	vertices.resize(vertexCount);
	for (uint32_t i = 0; i < m; ++i)
	{
		float z = halfDepth - i * dz;
		for (uint32_t j = 0; j < n; ++j)
		{
			float x = -halfWidth + j * dx;

			vertices[i * n + j].Position = float3(x, 0.0f, z);
			vertices[i * n + j].Normal = float3(0.0f, 1.0f, 0.0f);
			vertices[i * n + j].Tangent = float3(1.0f, 0.0f, 0.0f);

			// Stretch texture over grid.
			vertices[i * n + j].TexUV.x = j * du;
			vertices[i * n + j].TexUV.y = i * dv;
		}
	}

	//
	// Create the indices.
	//

	indices.resize(faceCount * 3); // 3 indices per face

	// Iterate over each quad and compute indices.
	uint32_t k = 0;
	for (uint32_t i = 0; i < m - 1; ++i)
	{
		for (uint32_t j = 0; j < n - 1; ++j)
		{
			indices[k] = i * n + j;
			indices[k + 1] = i * n + j + 1;
			indices[k + 2] = (i + 1) * n + j;

			indices[k + 3] = (i + 1) * n + j;
			indices[k + 4] = i * n + j + 1;
			indices[k + 5] = (i + 1) * n + j + 1;

			k += 6; // next quad
		}
	}
}

void PrimitiveMesh::ComputeSphere(Vector<Vertex>& vertices, Vector<uint32_t>& indices,
	float diameter, size_t tessellation) noexcept
{
	Assert(tessellation >= 3, "tesselation parameter out of range");

	vertices.clear();
	indices.clear();

	size_t verticalSegments = tessellation;
	size_t horizontalSegments = tessellation * 2;

	float radius = diameter / 2;

	// Create rings of vertices at progressively higher latitudes.
	for (size_t i = 0; i <= verticalSegments; i++)
	{
		float v = 1 - float(i) / verticalSegments;

		float latitude = (i * Math::PI / verticalSegments) - Math::PI_DIV_2;
		float dy, dxz;

		//XMScalarSinCos(&dy, &dxz, latitude);
		dy = sinf(latitude);
		dxz = cosf(latitude);

		// Create a single ring of vertices at this latitude.
		for (size_t j = 0; j <= horizontalSegments; j++)
		{
			float u = float(j) / horizontalSegments;

			float longitude = j * Math::TWO_PI / horizontalSegments;
			float dx, dz;

			//XMScalarSinCos(&dx, &dz, longitude);
			dx = sinf(longitude);
			dz = cosf(longitude);

			dx *= dxz;
			dz *= dxz;

			float3 normal(dx, dy, dz);
			float2 textureCoordinate(u, v);
			float3 tangent(0.0f, 0.0f, 0.0f);

			vertices.emplace_back(normal * radius, normal, textureCoordinate, tangent);
		}
	}

	// Fill the index buffer with triangles joining each pair of latitude rings.
	size_t stride = horizontalSegments + 1;

	for (size_t i = 0; i < verticalSegments; i++)
	{
		for (size_t j = 0; j <= horizontalSegments; j++)
		{
			size_t nextI = i + 1;
			size_t nextJ = (j + 1) % stride;

			indices.push_back((uint32_t)(i * stride + j));
			indices.push_back((uint32_t)(i * stride + nextJ));
			indices.push_back((uint32_t)(nextI * stride + j));

			indices.push_back((uint32_t)(i * stride + nextJ));
			indices.push_back((uint32_t)(nextI * stride + nextJ));
			indices.push_back((uint32_t)(nextI * stride + j));
		}
	}

	// compute the tangents
	ComputeMeshTangentVectors(vertices, indices);
}

void PrimitiveMesh::ComputeCylinder(Vector<Vertex>& vertices, Vector<uint32_t>& indices,
	float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount) noexcept
{
	vertices.clear();
	indices.clear();

	float stackHeight = height / stackCount;

	// Amount to increment radius as we move up each stack level from bottom to top.
	float radiusStep = (topRadius - bottomRadius) / stackCount;

	uint32_t ringCount = stackCount + 1;

	// Compute vertices for each stack ring starting at the bottom and moving up.
	for (uint32_t i = 0; i < ringCount; ++i)
	{
		float y = -0.5f * height + i * stackHeight;
		float r = bottomRadius + i * radiusStep;

		// vertices of ring
		float dTheta = Math::TWO_PI / sliceCount;
		for (uint32_t j = 0; j <= sliceCount; ++j)
		{
			float c = cosf(j * dTheta);
			float s = sinf(j * dTheta);

			float3 pos(r * c, y, r * s);
			float2 uv((float)j / sliceCount, 1.0f - (float)i / stackCount);

			// Cylinder can be parameterized as follows, where we introduce v
			// parameter that goes in the same direction as the v tex-coord
			// so that the bitangent goes in the same direction as the v tex-coord.
			//   Let r0 be the bottom radius and let r1 be the top radius.
			//   y(v) = h - hv for v in [0,1].
			//   r(v) = r1 + (r0-r1)v
			//
			//   x(t, v) = r(v)*cos(t)
			//   y(t, v) = h - hv
			//   z(t, v) = r(v)*sin(t)
			// 
			//  dx/dt = -r(v)*sin(t)
			//  dy/dt = 0
			//  dz/dt = +r(v)*cos(t)
			//
			//  dx/dv = (r0-r1)*cos(t)
			//  dy/dv = -h
			//  dz/dv = (r0-r1)*sin(t)

			// This is unit length.
			float3 tangent(-s, 0.0f, c);

			float dr = bottomRadius - topRadius;
			float3 bitangent(dr * c, -height, dr * s);

			float3 normal = tangent.cross(bitangent);
			normal.normalize();

			vertices.emplace_back(pos, normal, uv, tangent);
		}
	}

	// Add one because we duplicate the first and last vertex per ring
	// since the texture coordinates are different.
	uint32_t ringVertexCount = sliceCount + 1;

	// Compute indices for each stack.
	for (uint32_t i = 0; i < stackCount; ++i)
	{
		for (uint32_t j = 0; j < sliceCount; ++j)
		{
			indices.push_back(i * ringVertexCount + j);
			indices.push_back((i + 1) * ringVertexCount + j);
			indices.push_back((i + 1) * ringVertexCount + j + 1);

			indices.push_back(i * ringVertexCount + j);
			indices.push_back((i + 1) * ringVertexCount + j + 1);
			indices.push_back(i * ringVertexCount + j + 1);
		}
	}

	// 
	// Build top cap.
	//

	uint32_t baseIndex = (uint32_t)vertices.size();

	float y = 0.5f * height;
	float dTheta = Math::TWO_PI / sliceCount;

	// Duplicate cap ring vertices because the texture coordinates and normals differ.
	for (uint32_t i = 0; i <= sliceCount; ++i)
	{
		float x = topRadius * cosf(i * dTheta);
		float z = topRadius * sinf(i * dTheta);

		// Scale down by the height to try and make top cap texture coord area
		// proportional to base.
		float u = x / height + 0.5f;
		float v = z / height + 0.5f;

		vertices.emplace_back(float3(x, y, z), float3(0.0f, 1.0f, 0.0f), float2(u, v), float3(1.0f, 0.0f, 0.0f));
	}

	// Cap center vertex.
	vertices.emplace_back(float3(0.0f, y, 0.0f), float3(0.0f, 1.0f, 0.0f), float2(0.5f, 0.5f), float3(1.0f, 0.0f, 0.0f));

	// Index of center vertex.
	uint32_t centerIndex = (uint32_t)vertices.size() - 1;

	for (uint32_t i = 0; i < sliceCount; ++i)
	{
		indices.push_back(centerIndex);
		indices.push_back(baseIndex + i + 1);
		indices.push_back(baseIndex + i);
	}

	// 
	// Build bottom cap.
	//

	baseIndex = (uint32_t)vertices.size();
	y = -0.5f * height;

	// vertices of ring
	dTheta = Math::TWO_PI / sliceCount;
	for (uint32_t i = 0; i <= sliceCount; ++i)
	{
		float x = bottomRadius * cosf(i * dTheta);
		float z = bottomRadius * sinf(i * dTheta);

		// Scale down by the height to try and make top cap texture coord area
		// proportional to base.
		float u = x / height + 0.5f;
		float v = z / height + 0.5f;

		vertices.emplace_back(float3(x, y, z), float3(0.0f, -1.0f, 0.0f), float2(u, v), float3(1.0f, 0.0f, 0.0f));
	}

	// Cap center vertex.
	vertices.emplace_back(float3(0.0f, y, 0.0f), float3(0.0f, -1.0f, 0.0f), float2(0.5f, 0.5f), float3(1.0f, 0.0f, 0.0f));

	// Cache the index of center vertex.
	centerIndex = (uint32_t)vertices.size() - 1;

	for (uint32_t i = 0; i < sliceCount; ++i)
	{
		indices.push_back(centerIndex);
		indices.push_back(baseIndex + i);
		indices.push_back(baseIndex + i + 1);
	}
}

void PrimitiveMesh::ComputeCone(Vector<Vertex>& vertices, Vector<uint32_t>& indices,
	float diameter, float height, size_t tessellation) noexcept
{
	Assert(tessellation >= 3, "tesselation parameter out of range");

	vertices.clear();
	indices.clear();

	height /= 2;

	float3 topOffset(0.0f, height, 0.0f);

	float radius = diameter / 2;
	size_t stride = tessellation + 1;

	// Create a ring of triangles around the outside of the cone.
	for (size_t i = 0; i <= tessellation; i++)
	{
		float angle = i * Math::TWO_PI / tessellation;

		float3 circlevec(sinf(angle), 0, cosf(angle));
		float3 sideOffset = circlevec * radius;

		float u = float(i) / tessellation;

		float3 pt = sideOffset - topOffset;

		angle = (i * Math::TWO_PI / tessellation) + Math::PI_DIV_2;
		float3 t(sinf(angle), 0.0f, cosf(angle));

		float3 normal = t.cross(topOffset - pt);
		normal.normalize();

		// Duplicate the top vertex for distinct normals
		vertices.emplace_back(topOffset, normal, float2(0.0f, 0.0f), float3(0.0f, 0.0f, 0.0f));
		vertices.emplace_back(pt, normal, float2(u, 1.0f), float3(0.0f, 0.0f, 0.0f));

		indices.push_back((uint32_t)(i * 2));
		indices.push_back((uint32_t)((i * 2 + 3) % (stride * 2)));
		indices.push_back((uint32_t)((i * 2 + 1) % (stride * 2)));
	}

	// Create flat triangle fan caps to seal the bottom.
	//CreateCylinderCap(vertices, indices, tessellation, height, radius, false);

	// Create cap indices.
	for (size_t i = 0; i < tessellation - 2; i++)
	{
		size_t i1 = (i + 1) % tessellation;
		size_t i2 = (i + 2) % tessellation;

		size_t vbase = vertices.size();
		indices.push_back((uint32_t)vbase);
		indices.push_back((uint32_t)(vbase + i1));
		indices.push_back((uint32_t)(vbase + i2));
	}

	// Which end of the cylinder is this?
	float3 normal(0.0f, -1.0f, 0.0f);
	float2 textureScale(0.5f, -0.5f);

	// Create cap vertices.
	for (size_t i = 0; i < tessellation; i++)
	{
		float angle = i * Math::TWO_PI / tessellation;
		float3 circleVector(sinf(angle), 0.0f, cosf(angle));

		float3 position = (circleVector * radius) + (normal * height);
		float2 textureCoordinate = float2(circleVector.x, circleVector.y) * textureScale + float2(0.5f, 0.5f);

		vertices.emplace_back(position, normal, textureCoordinate, float3(0.0f, 0.0f, 0.0f));
	}

	// compute the tangents
	ComputeMeshTangentVectors(vertices, indices);
}

void PrimitiveMesh::ComputeTorus(Vector<Vertex>& vertices, Vector<uint32_t>& indices,
	float diameter, float thickness, size_t tessellation) noexcept
{
	vertices.clear();
	indices.clear();

	Assert(tessellation >= 3, "tesselation parameter out of range");

	size_t stride = tessellation + 1;

	// First we loop around the main ring of the torus.
	for (size_t i = 0; i <= tessellation; i++)
	{
		float u = float(i) / tessellation;

		float outerAngle = i * Math::TWO_PI / tessellation - Math::PI_DIV_2;

		// Create a transform matrix that will align geometry to
		// slice perpendicularly though the current ring position.
		//XMMATRIX transform = XMMatrixTranslation(diameter / 2, 0, 0) * XMMatrixRotationY(outerAngle);
		v_float4x4 vTransform = mul(translate(float4a(diameter / 2, 0.0f, 0.0f, 0.0f)), rotateY(outerAngle));

		// Now we loop along the other axis, around the side of the tube.
		for (size_t j = 0; j <= tessellation; j++)
		{
			float v = 1 - float(j) / tessellation;

			float innerAngle = j * Math::TWO_PI / tessellation + Math::PI;

			// Create a vertex.
			float4a normal(cosf(innerAngle), sinf(innerAngle), 0.0f, 0.0f);
			float thicknessDiv2 = (thickness / 2);
			float4a position(normal.x * thicknessDiv2, normal.y * thicknessDiv2, normal.z * thicknessDiv2, 1.0f);
			float2 textureCoordinate(u, v);

			//position = XMVector3Transform(position, transform);
			__m128 vPos = _mm_load_ps(reinterpret_cast<float*>(&position));
			vPos = mul(vTransform, vPos);
			_mm_store_ps(reinterpret_cast<float*>(&position), vPos);

			//normal = XMVector3TransformNormal(normal, transform);
			__m128 vN = _mm_load_ps(reinterpret_cast<float*>(&normal));
			vN = mul(vTransform, vN);
			vN = normalize(vN);
			_mm_store_ps(reinterpret_cast<float*>(&normal), vN);

			vertices.emplace_back(float3(position.x, position.y, position.z), float3(normal.x, normal.y, normal.z),
				textureCoordinate, float3(0.0f, 0.0f, 0.0f));

			// And create indices for two triangles.
			size_t nextI = (i + 1) % stride;
			size_t nextJ = (j + 1) % stride;

			indices.push_back((uint32_t)(i * stride + j));
			indices.push_back((uint32_t)(i * stride + nextJ));
			indices.push_back((uint32_t)(nextI * stride + j));

			indices.push_back((uint32_t)(i * stride + nextJ));
			indices.push_back((uint32_t)(nextI * stride + nextJ));
			indices.push_back((uint32_t)(nextI * stride + j));
		}
	}

	// compute the tangents
	ComputeMeshTangentVectors(vertices, indices);
}

namespace
{
	const float3 TeapotControlPoints[] =
	{
		{ 0, 0.345f, -0.05f },
		{ -0.028f, 0.345f, -0.05f },
		{ -0.05f, 0.345f, -0.028f },
		{ -0.05f, 0.345f, -0 },
		{ 0, 0.3028125f, -0.334375f },
		{ -0.18725f, 0.3028125f, -0.334375f },
		{ -0.334375f, 0.3028125f, -0.18725f },
		{ -0.334375f, 0.3028125f, -0 },
		{ 0, 0.3028125f, -0.359375f },
		{ -0.20125f, 0.3028125f, -0.359375f },
		{ -0.359375f, 0.3028125f, -0.20125f },
		{ -0.359375f, 0.3028125f, -0 },
		{ 0, 0.27f, -0.375f },
		{ -0.21f, 0.27f, -0.375f },
		{ -0.375f, 0.27f, -0.21f },
		{ -0.375f, 0.27f, -0 },
		{ 0, 0.13875f, -0.4375f },
		{ -0.245f, 0.13875f, -0.4375f },
		{ -0.4375f, 0.13875f, -0.245f },
		{ -0.4375f, 0.13875f, -0 },
		{ 0, 0.007499993f, -0.5f },
		{ -0.28f, 0.007499993f, -0.5f },
		{ -0.5f, 0.007499993f, -0.28f },
		{ -0.5f, 0.007499993f, -0 },
		{ 0, -0.105f, -0.5f },
		{ -0.28f, -0.105f, -0.5f },
		{ -0.5f, -0.105f, -0.28f },
		{ -0.5f, -0.105f, -0 },
		{ 0, -0.105f, 0.5f },
		{ 0, -0.2175f, -0.5f },
		{ -0.28f, -0.2175f, -0.5f },
		{ -0.5f, -0.2175f, -0.28f },
		{ -0.5f, -0.2175f, -0 },
		{ 0, -0.27375f, -0.375f },
		{ -0.21f, -0.27375f, -0.375f },
		{ -0.375f, -0.27375f, -0.21f },
		{ -0.375f, -0.27375f, -0 },
		{ 0, -0.2925f, -0.375f },
		{ -0.21f, -0.2925f, -0.375f },
		{ -0.375f, -0.2925f, -0.21f },
		{ -0.375f, -0.2925f, -0 },
		{ 0, 0.17625f, 0.4f },
		{ -0.075f, 0.17625f, 0.4f },
		{ -0.075f, 0.2325f, 0.375f },
		{ 0, 0.2325f, 0.375f },
		{ 0, 0.17625f, 0.575f },
		{ -0.075f, 0.17625f, 0.575f },
		{ -0.075f, 0.2325f, 0.625f },
		{ 0, 0.2325f, 0.625f },
		{ 0, 0.17625f, 0.675f },
		{ -0.075f, 0.17625f, 0.675f },
		{ -0.075f, 0.2325f, 0.75f },
		{ 0, 0.2325f, 0.75f },
		{ 0, 0.12f, 0.675f },
		{ -0.075f, 0.12f, 0.675f },
		{ -0.075f, 0.12f, 0.75f },
		{ 0, 0.12f, 0.75f },
		{ 0, 0.06375f, 0.675f },
		{ -0.075f, 0.06375f, 0.675f },
		{ -0.075f, 0.007499993f, 0.75f },
		{ 0, 0.007499993f, 0.75f },
		{ 0, -0.04875001f, 0.625f },
		{ -0.075f, -0.04875001f, 0.625f },
		{ -0.075f, -0.09562501f, 0.6625f },
		{ 0, -0.09562501f, 0.6625f },
		{ -0.075f, -0.105f, 0.5f },
		{ -0.075f, -0.18f, 0.475f },
		{ 0, -0.18f, 0.475f },
		{ 0, 0.02624997f, -0.425f },
		{ -0.165f, 0.02624997f, -0.425f },
		{ -0.165f, -0.18f, -0.425f },
		{ 0, -0.18f, -0.425f },
		{ 0, 0.02624997f, -0.65f },
		{ -0.165f, 0.02624997f, -0.65f },
		{ -0.165f, -0.12375f, -0.775f },
		{ 0, -0.12375f, -0.775f },
		{ 0, 0.195f, -0.575f },
		{ -0.0625f, 0.195f, -0.575f },
		{ -0.0625f, 0.17625f, -0.6f },
		{ 0, 0.17625f, -0.6f },
		{ 0, 0.27f, -0.675f },
		{ -0.0625f, 0.27f, -0.675f },
		{ -0.0625f, 0.27f, -0.825f },
		{ 0, 0.27f, -0.825f },
		{ 0, 0.28875f, -0.7f },
		{ -0.0625f, 0.28875f, -0.7f },
		{ -0.0625f, 0.2934375f, -0.88125f },
		{ 0, 0.2934375f, -0.88125f },
		{ 0, 0.28875f, -0.725f },
		{ -0.0375f, 0.28875f, -0.725f },
		{ -0.0375f, 0.298125f, -0.8625f },
		{ 0, 0.298125f, -0.8625f },
		{ 0, 0.27f, -0.7f },
		{ -0.0375f, 0.27f, -0.7f },
		{ -0.0375f, 0.27f, -0.8f },
		{ 0, 0.27f, -0.8f },
		{ 0, 0.4575f, -0 },
		{ 0, 0.4575f, -0.2f },
		{ -0.1125f, 0.4575f, -0.2f },
		{ -0.2f, 0.4575f, -0.1125f },
		{ -0.2f, 0.4575f, -0 },
		{ 0, 0.3825f, -0 },
		{ 0, 0.27f, -0.35f },
		{ -0.196f, 0.27f, -0.35f },
		{ -0.35f, 0.27f, -0.196f },
		{ -0.35f, 0.27f, -0 },
		{ 0, 0.3075f, -0.1f },
		{ -0.056f, 0.3075f, -0.1f },
		{ -0.1f, 0.3075f, -0.056f },
		{ -0.1f, 0.3075f, -0 },
		{ 0, 0.3075f, -0.325f },
		{ -0.182f, 0.3075f, -0.325f },
		{ -0.325f, 0.3075f, -0.182f },
		{ -0.325f, 0.3075f, -0 },
		{ 0, 0.27f, -0.325f },
		{ -0.182f, 0.27f, -0.325f },
		{ -0.325f, 0.27f, -0.182f },
		{ -0.325f, 0.27f, -0 },
		{ 0, -0.33f, -0 },
		{ -0.1995f, -0.33f, -0.35625f },
		{ 0, -0.31125f, -0.375f },
		{ 0, -0.33f, -0.35625f },
		{ -0.35625f, -0.33f, -0.1995f },
		{ -0.375f, -0.31125f, -0 },
		{ -0.35625f, -0.33f, -0 },
		{ -0.21f, -0.31125f, -0.375f },
		{ -0.375f, -0.31125f, -0.21f },
	};

	struct TeapotPatch
	{
		bool mirrorZ;
		int indices[16];
	};

	const TeapotPatch TeapotPatches[] =
	{
		// Rim.
		{ true, { 102, 103, 104, 105, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } },

		// Body.
		{ true, { 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 } },
		{ true, { 24, 25, 26, 27, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40 } },

		// Lid.
		{ true, { 96, 96, 96, 96, 97, 98, 99, 100, 101, 101, 101, 101, 0, 1, 2, 3 } },
		{ true, { 0, 1, 2, 3, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117 } },

		// Handle.
		{ false, { 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56 } },
		{ false, { 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 28, 65, 66, 67 } },

		// Spout.
		{ false, { 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83 } },
		{ false, { 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95 } },

		// Bottom.
		{ true, { 118, 118, 118, 118, 124, 122, 119, 121, 123, 126, 125, 120, 40, 39, 38, 37 } },
	};

	float3 CubicInterpolate(float3 p1, float3 p2, float3 p3, float3 p4, float t) noexcept
	{
		float T0 = (1 - t) * (1 - t) * (1 - t);
		float T1 = 3 * t * (1 - t) * (1 - t);
		float T2 = 3 * t * t * (1 - t);
		float T3 = t * t * t;

		float3 result = p1 * T0 + p2 * T1 + p3 * T2 + p4 * T3;

		return result;
	}

	float3 CubicTangent(float3 p1, float3 p2, float3 p3, float3 p4, float t) noexcept
	{
		return p1 * (-1 + 2 * t - t * t) +
			p2 * (1 - 4 * t + 3 * t * t) +
			p3 * (2 * t - 3 * t * t) +
			p4 * (t * t);
	}

	void CreatePatchVertices(Vector<Vertex>& vertices, float3 patch[16],
		size_t tessellation, bool isMirrored) noexcept
	{
		for (size_t i = 0; i <= tessellation; i++)
		{
			float u = float(i) / tessellation;

			for (size_t j = 0; j <= tessellation; j++)
			{
				float v = float(j) / tessellation;

				// Perform four horizontal bezier interpolations
				// between the control points of this patch.
				float3 p1 = CubicInterpolate(patch[0], patch[1], patch[2], patch[3], u);
				float3 p2 = CubicInterpolate(patch[4], patch[5], patch[6], patch[7], u);
				float3 p3 = CubicInterpolate(patch[8], patch[9], patch[10], patch[11], u);
				float3 p4 = CubicInterpolate(patch[12], patch[13], patch[14], patch[15], u);

				// Perform a vertical interpolation between the results of the
				// previous horizontal interpolations, to compute the position.
				float3 position = CubicInterpolate(p1, p2, p3, p4, v);

				// Perform another four bezier interpolations between the control
				// points, but this time vertically rather than horizontally.
				float3 q1 = CubicInterpolate(patch[0], patch[4], patch[8], patch[12], v);
				float3 q2 = CubicInterpolate(patch[1], patch[5], patch[9], patch[13], v);
				float3 q3 = CubicInterpolate(patch[2], patch[6], patch[10], patch[14], v);
				float3 q4 = CubicInterpolate(patch[3], patch[7], patch[11], patch[15], v);

				// Compute vertical and horizontal tangent vectors.
				float3 tangent1 = CubicTangent(p1, p2, p3, p4, v);
				float3 tangent2 = CubicTangent(q1, q2, q3, q4, u);

				// Cross the two tangent vectors to compute the normal.
				float3 normal = tangent1.cross(tangent2);

				//if (!XMVector3NearEqual(normal, XMVectorZero(), g_XMEpsilon))
				if (abs(normal.x) > FLT_EPSILON || abs(normal.y) > FLT_EPSILON || abs(normal.z) > FLT_EPSILON)
				{
					normal.normalize();

					// If this patch is mirrored, we must invert the normal.
					if (isMirrored)
					{
						normal = -normal;
					}
				}
				else
				{
					// In a tidy and well constructed bezier patch, the preceding
					// normal computation will always work. But the classic teapot
					// model is not tidy or well constructed! At the top and bottom
					// of the teapot, it contains degenerate geometry where a patch
					// has several control points in the same place, which causes
					// the tangent computation to fail and produce a zero normal.
					// We 'fix' these cases by just hard-coding a normal that points
					// either straight up or straight down, depending on whether we
					// are on the top or bottom of the teapot. This is not a robust
					// solution for all possible degenerate bezier patches, but hey,
					// it's good enough to make the teapot work correctly!

					// { 0.0f, 1.0f, 0.0f, 0.0f }
					// { 0.0f, -1.0f, 0.0f, 0.0f }
					//normal = XMVectorSelect(g_XMIdentityR1, g_XMNegIdentityR1, XMVectorLess(position, XMVectorZero()));

					if (position.y < 0.0f)
						normal = float3(0.0f, -1.0f, 0.0f);
					else
						normal = float3(0.0f, 1.0f, 0.0f);
				}

				// Compute the texture coordinate.
				float mirroredU = isMirrored ? 1 - u : u;
				float2 textureCoordinate(mirroredU, v);

				// Output this vertex.
				vertices.emplace_back(position, -normal, textureCoordinate, float3(0.0f, 0.0f, 0.0f));
			}
		}
	}

	void CreatePatchIndices(Vector<uint32_t>& indices, size_t vbase, size_t tessellation, bool isMirrored)
	{
		size_t stride = tessellation + 1;

		for (size_t i = 0; i < tessellation; i++)
		{
			for (size_t j = 0; j < tessellation; j++)
			{
				// Make a list of six index values (two triangles).
				size_t indexVals[6] =
				{
					// [had to reorder the indices from 0, 1, 2 to 0, 2, 1]
					i * stride + j,
					(i + 1) * stride + j + 1,
					(i + 1) * stride + j,

					// [had to reorder the indices from 0, 1, 2 to 0, 2, 1]
					i * stride + j,
					i * stride + j + 1,
					(i + 1) * stride + j + 1,
				};

				// If this patch is mirrored, reverse indices to fix the winding order.
				//if (isMirrored)
				//{
				//	std::reverse(indices.begin(), indices.end());
				//}

				// Output these index values.
				if (!isMirrored)
				{
					for (int k = 0; k < 6; k++)
						indices.push_back((uint32_t)(vbase + indexVals[k]));
				}
				else
				{
					for (int k = 5; k >= 0; k--)
						indices.push_back((uint32_t)(vbase + indexVals[k]));
				}
			}
		}
	}

	// Tessellates the specified bezier patch.
	void TessellatePatch(Vector<Vertex>& vertices, Vector<uint32_t>& indices,
		const TeapotPatch& patch, size_t tessellation, float3 scale, bool isMirrored)
	{
		// Look up the 16 control points for this patch.
		float3 controlPoints[16];

		for (int i = 0; i < 16; i++)
		{
			controlPoints[i] = TeapotControlPoints[patch.indices[i]] * scale;
		}

		// Create the index data.
		size_t vbase = vertices.size();
		CreatePatchIndices(indices, vbase, tessellation, isMirrored);

		// Create the vertex data.
		CreatePatchVertices(vertices, controlPoints, tessellation, isMirrored);
	}
}

void PrimitiveMesh::ComputeTeapot(Vector<Vertex>& vertices, Vector<uint32_t>& indices,
	float size, size_t tessellation) noexcept
{
	vertices.clear();
	indices.clear();

	Assert(tessellation >= 1, "tesselation parameter out of range");

	float3 scaleNegateX = size * float3(-1.0f, 1.0f, 1.0f);
	float3 scaleNegateZ = size * float3(1.0f, 1.0f, -1.0f);
	float3 scaleNegateXZ = size * float3(-1.0f, 1.0f, -1.0f);

	for (size_t i = 0; i < sizeof(TeapotPatches) / sizeof(TeapotPatch); i++)
	{
		TeapotPatch const& patch = TeapotPatches[i];

		// Because the teapot is symmetrical from left to right, we only store
		// data for one side, then tessellate each patch twice, mirroring in X.
		TessellatePatch(vertices, indices, patch, tessellation, float3(size, size, size), false);
		TessellatePatch(vertices, indices, patch, tessellation, scaleNegateX, true);

		if (patch.mirrorZ)
		{
			// Some parts of the teapot (the body, lid, and rim, but not the
			// handle or spout) are also symmetrical from front to back, so
			// we tessellate them four times, mirroring in Z as well as X.
			TessellatePatch(vertices, indices, patch, tessellation, scaleNegateZ, true);
			TessellatePatch(vertices, indices, patch, tessellation, scaleNegateXZ, false);
		}
	}
}


