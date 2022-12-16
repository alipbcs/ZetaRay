#pragma once

#include "../RenderPass.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass
{
	struct ClearPass final
	{
		enum SHADER_IN_DESC
		{
			RTV,
			DEPTH_BUFFER,
			BASE_COLOR,
			NORMAL,
			METALNESS_ROUGHNESS,
			MOTION_VECTOR,
			EMISSIVE_COLOR,
			HDR_LIGHT_ACCUM,
			COUNT
		};

		ClearPass() noexcept = default;
		~ClearPass() noexcept = default;

		void SetDescriptor(int i, D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
		{
			Assert(i < SHADER_IN_DESC::COUNT, "out-of-bound access.");
			m_descriptors[i] = h;
		}

		void Clear(Core::CommandList& cmdList) noexcept;

	private:
		D3D12_CPU_DESCRIPTOR_HANDLE m_descriptors[SHADER_IN_DESC::COUNT] = { 0 };
	};
}