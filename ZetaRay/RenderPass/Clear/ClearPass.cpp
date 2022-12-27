#include "ClearPass.h"
#include "../../Utility/Error.h"
#include "../../Core/CommandList.h"
#include "../../Core/Constants.h"

using namespace ZetaRay;
using namespace ZetaRay::RenderPass;

void ClearPass::Clear(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	directCmdList.PIXBeginEvent("Clear");

	// while other RTVs are optional, depth buffer must always be cleared
	Assert(m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER].ptr > 0, "DSV hasn't been set");

	if(m_descriptors[SHADER_IN_DESC::BASE_COLOR].ptr)
		directCmdList.ClearRenderTargetView(m_descriptors[SHADER_IN_DESC::BASE_COLOR],
			0.0f, 0.0f, 0.0f, 0.0f);

	if (m_descriptors[SHADER_IN_DESC::NORMAL_CURV].ptr)
		directCmdList.ClearRenderTargetView(m_descriptors[SHADER_IN_DESC::NORMAL_CURV],
			0.0f, 0.0f, 0.0f, 0.0f);

	if (m_descriptors[SHADER_IN_DESC::METALNESS_ROUGHNESS].ptr)
		directCmdList.ClearRenderTargetView(m_descriptors[SHADER_IN_DESC::METALNESS_ROUGHNESS],
			0.0f, 0.0f, 0.0f, 0.0f);

	if (m_descriptors[SHADER_IN_DESC::MOTION_VECTOR].ptr)
		directCmdList.ClearRenderTargetView(m_descriptors[SHADER_IN_DESC::MOTION_VECTOR],
			0.0f, 0.0f, 0.0f, 0.0f);

	if (m_descriptors[SHADER_IN_DESC::EMISSIVE_COLOR].ptr)
		directCmdList.ClearRenderTargetView(m_descriptors[SHADER_IN_DESC::EMISSIVE_COLOR],
			0.0f, 0.0f, 0.0f, 0.0f);

	if (m_descriptors[SHADER_IN_DESC::HDR_LIGHT_ACCUM].ptr)
		directCmdList.ClearRenderTargetView(m_descriptors[SHADER_IN_DESC::HDR_LIGHT_ACCUM],
			0.0f, 0.0f, 0.0f, 0.0f);

	//directCmdList.ClearDepthStencilView(m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER], D3D12_CLEAR_FLAG_DEPTH, 1.0f);
	constexpr float clearVal = RendererConstants::USE_REVERSE_Z ? 0.0f : 1.0f;
	directCmdList.ClearDepthStencilView(m_descriptors[SHADER_IN_DESC::DEPTH_BUFFER], D3D12_CLEAR_FLAG_DEPTH, clearVal);

	directCmdList.PIXEndEvent();
}
