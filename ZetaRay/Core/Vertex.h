#pragma once

#include "Device.h"
#include "../Math/Vector.h"
#include "../RenderPass/Common/HLSLCompat.h"

namespace ZetaRay
{
#if USE_16_BIT_INDICES
	static const DXGI_FORMAT MESH_INDEX_FORMAT = DXGI_FORMAT_R16_UINT;
#else
	static const DXGI_FORMAT MESH_INDEX_FORMAT = DXGI_FORMAT_R32_UINT;
#endif // 

	struct VertexPosNormalTexTangent
	{
		VertexPosNormalTexTangent() noexcept = default;

		VertexPosNormalTexTangent(const Math::float3& position,
			const Math::float3& normal, 
			const Math::float2& texUV, 
			const Math::float3& tangent) noexcept
			: Position(position),
			Normal(normal),
			TexUV(texUV),
			Tangent(tangent)
		{}

		Math::float3 Position;
		Math::float3 Normal;
		Math::float2 TexUV;
		Math::float3 Tangent;

		static constexpr int NumInputElements = 4;
		static constexpr D3D12_INPUT_ELEMENT_DESC InputElements[NumInputElements] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		static constexpr D3D12_INPUT_LAYOUT_DESC InputLayout =
		{
			VertexPosNormalTexTangent::InputElements,
			VertexPosNormalTexTangent::NumInputElements
		};
	};
}