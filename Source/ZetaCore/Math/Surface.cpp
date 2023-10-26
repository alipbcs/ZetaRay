#include "Surface.h"
#include "../App/Log.h"

using namespace ZetaRay::Core;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// Surfaces
//--------------------------------------------------------------------------------------

void ZetaRay::Math::ComputeMeshTangentVectors(MutableSpan<Vertex> vertices, Span<uint32_t> indices, bool rhsIndices)
{
	float3* tangents = new float3[vertices.size()];
	memset(tangents, 0, sizeof(float3) * vertices.size());

//	for (auto& v : vertices)
//		v.Tangent = half3(0.0f, 0.0f, 0.0f);

	// Given triangle with vertices v0, v1, v2 (in clockwise order) and corresponding texture coords
	// (u0, v0), (u1, v1) and (u2, v2) we have:
	// 
	//    p1 - p0 = (u1 - u0) * T + (v1 - v0) * B
	//    p2 - p0 = (u2 - u0) * T + (v2 - v0) * B
	//
	// In matrix form:
	// 
	//		   [ u1 - u0  u2 - u0 ]
	// [T B] * |                  | = [p1 - p0  p2 - p0]
	//		   [ v1 - v0  v2 - v0 ]
	//
	// This linear system is solved with:
	// 
	// |     |			  |                  |                                    
	// | T B | = 1 / D	* | p1 - p0  p2 - p0 |  *  | v2 - v0  u0 - u2 |
	// |     |			  |                  |     | v0 - v1  u1 - u0 |           
	//
	// where D = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)

	uint32_t numCollinearTris = 0;

	for (size_t i = 0; i < indices.size(); i += 3)
	{
		uint32_t i0 = indices[i];
		uint32_t i1 = indices[i + 1];
		uint32_t i2 = indices[i + 2];

		// swap i1 & i2
		if (rhsIndices)
		{
			uint32_t temp = i1;
			i1 = i2;
			i2 = temp;
		}

		float2 uv0 = vertices[i0].TexUV;
		float3 pos0 = vertices[i0].Position;

		float2 uv1 = vertices[i1].TexUV;
		float3 pos1 = vertices[i1].Position;

		float2 uv2 = vertices[i2].TexUV;
		float3 pos2 = vertices[i2].Position;

		float2 uv1Minuv0 = uv1 - uv0;
		float2 uv2Minuv0 = uv2 - uv0;

		float det = uv1Minuv0.x * uv2Minuv0.y - uv1Minuv0.y * uv2Minuv0.x;
		if (det == 0)
		{
			numCollinearTris++;
			continue;
		}

		float oneDivDet = 1.0f / det;

		float3 p1Minp0 = pos1 - pos0;
		float3 p2Minp0 = pos2 - pos0;

		float3 T(p1Minp0.x * uv2Minuv0.y + p2Minp0.x * -uv1Minuv0.y,
				 p1Minp0.y * uv2Minuv0.y + p2Minp0.y * -uv1Minuv0.y,
				 p1Minp0.z * uv2Minuv0.y + p2Minp0.z * -uv1Minuv0.y);

		T *= oneDivDet;

		tangents[i0] += T;
		tangents[i1] += T;
		tangents[i2] += T;
	}

	// Gram–Schmidt Orthonormalization
	// assumes vertex normala are normalized
	for(size_t i = 0; i < vertices.size(); i++)
	{
		float3 n = float3(vertices[i].Normal);
		float3 tangProjectedOnNormal = n.dot(tangents[i]) * n;
		tangents[i] -= tangProjectedOnNormal;
		tangents[i].normalize();

		vertices[i].Tangent = snorm3(tangents[i]);
	}

	if (numCollinearTris)
	{
		LOG_UI_WARNING("Mesh had %u/%u collinear triangles, vertex tangents might be missing.\n",
			numCollinearTris, (uint32_t)indices.size() / 3);
	}

	delete[] tangents;
}

//void Math::MergeBoundingBoxes(BoundingBox& out, const BoundingBox& b1, const BoundingBox& b2)
//{
//	// NOTE: DirectX::BoundingBox's default constructor initializes center to (0, 0, 0)
//	// and extents to (1, 1, 1). When a non-defualt AABB is merged with a default-contructed one, 
//	// intuitively, we might expect the result to be just the latter AABB. But due to the default values
//	// for center and extents, this is not neccessarilty the case.
//	const float EPS = 1e-4;
//	const bool b1Degenerate = b1.Extents.x < EPS || b1.Extents.y < EPS || b1.Extents.z < EPS;
//	const bool b2Degenerate = b2.Extents.x < EPS || b2.Extents.y < EPS || b2.Extents.z < EPS;
//
//	Check(!b1Degenerate || !b2Degenerate, "Degenerate AABBs in the inputs.", SEVERITY::FATAL);
//
//	BoundingBox merged;
//	BoundingBox::CreateMerged(merged, b1, b2);
//
//	out = b1Degenerate ? b2 : (b2Degenerate ? b1 : merged);
//
//	// release build breaks because of these lines
////#ifdef _DEBUG
////	if (b1Empty)
////	{
////		out = b2;
////		return;
////	}
////
////	if (b2Empty)
////	{
////		out = b1;
////		return;
////	}
////
////#endif // _DEBUG
//}

