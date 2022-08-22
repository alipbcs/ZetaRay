#include "Surface.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// Surfaces
//--------------------------------------------------------------------------------------

bool Math::ComputeMeshTangentVectors(Vector<VertexPosNormalTexTangent>& vertices,
	const Vector<INDEX_TYPE>& indices, bool rhsIndices) noexcept
{
	for (auto& v : vertices)
		v.Tangent = float3(0.0f, 0.0f, 0.0f);

	// Given triangle with vertices v0, v1, v2 (in clockwise order):
	//    p1 - p0 = (u1 - u0) * T + (v1 - v0) * B
	//    p2 - p0 = (u2 - u0) * T + (v2 - v0) * B
	//
	// In matrix form:
	// |     |                         |                  |
	// | T B |  | u1 - u0  u2 - u0 | = | p1 - p0  p2 - p0 |
	// |     |  | v1 - v0  v2 - v0 |   |                  |
	//
	// Solving for first matrix:
	// |     |			  |                  |                                    
	// | T B | = 1 / D	* | p1 - p0  p2 - p0 |  *  | v2 - v0  u0 - u2 |
	// |     |			  |                  |     | v0 - v1  u1 - u0 |           
	//
	// where D = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)

	for (size_t i = 0; i < indices.size(); i += 3)
	{
		INDEX_TYPE i0 = indices[i];
		INDEX_TYPE i1 = indices[i + 1];
		INDEX_TYPE i2 = indices[i + 2];

		// swap i1 & i2
		if (rhsIndices)
		{
			INDEX_TYPE temp = i1;
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
		//Check(det != 0, "triangle uv coords are colinear");
		if (det == 0)
			return false;

		float oneDivDet = 1.0f / det;

		float3 p1Minp0 = pos1 - pos0;
		float3 p2Minp0 = pos2 - pos0;

		float3 T(p1Minp0.x * uv2Minuv0.y + p2Minp0.x * -uv1Minuv0.y,
				 p1Minp0.y * uv2Minuv0.y + p2Minp0.y * -uv1Minuv0.y,
				 p1Minp0.z * uv2Minuv0.y + p2Minp0.z * -uv1Minuv0.y);

		T *= oneDivDet;

		vertices[i0].Tangent += T;
		vertices[i1].Tangent += T;
		vertices[i2].Tangent += T;
	}

	// Gram–Schmidt Orthonormalization
	// assumes vertex normala are normalized
	for (auto& vertex : vertices)
	{
		float3 tangProjectedOnNormal = vertex.Normal.dot(vertex.Tangent) * vertex.Normal;
		vertex.Tangent -= tangProjectedOnNormal;
		vertex.Tangent.normalize();
	}

	return true;
}

//void Math::MergeBoundingBoxes(BoundingBox& out, const BoundingBox& b1, const BoundingBox& b2) noexcept
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

