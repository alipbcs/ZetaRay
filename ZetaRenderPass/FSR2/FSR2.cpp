#include "FSR2.h"
#include "Backend.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <App/App.h>

using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass::FSR2_Internal;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// FSR2
//--------------------------------------------------------------------------------------

FSR2Pass::~FSR2Pass() noexcept
{
	Reset();
}

void FSR2Pass::Init() noexcept
{
	auto& renderer = App::GetRenderer();
	const int w = renderer.GetDisplayWidth();
	const int h = renderer.GetDisplayHeight();

	FSR2_Internal::Init(UPSCALED_RES_FORMAT, w, h);
}

bool FSR2Pass::IsInitialized() noexcept
{
	return FSR2_Internal::IsInitialized();
}

void FSR2Pass::OnWindowResized() noexcept
{
	auto& renderer = App::GetRenderer();
	const int w = renderer.GetDisplayWidth();
	const int h = renderer.GetDisplayHeight();

	Assert(IsInitialized(), "FSR2 backend hasn't been initialized.");
	Reset();

	FSR2_Internal::Init(UPSCALED_RES_FORMAT, w, h);

	//LOG("FSR2Pass::OnWindowResized()");
}

void FSR2Pass::Reset() noexcept
{
	if (IsInitialized())
	{
		FSR2_Internal::Shutdown();

#ifdef _DEBUG
		memset(&m_inputResources, 0, sizeof(m_inputResources));
#endif // _DEBUG
	}
}

void FSR2Pass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
		cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");

	ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

	Assert(m_inputResources[(int)SHADER_IN_RES::COLOR], "Color input res hasn't been set.");
	Assert(m_inputResources[(int)SHADER_IN_RES::DEPTH], "Depth buffer res hasn't been set.");
	Assert(m_inputResources[(int)SHADER_IN_RES::MOTION_VECTOR], "Motion vectors res hasn't been set.");

	computeCmdList.PIXBeginEvent("FSR2");

	FSR2_Internal::DispatchParams params;
	params.Color = m_inputResources[(int)SHADER_IN_RES::COLOR];
	params.DepthBuffer = m_inputResources[(int)SHADER_IN_RES::DEPTH];
	params.MotionVectors = m_inputResources[(int)SHADER_IN_RES::MOTION_VECTOR];

	FSR2_Internal::Dispatch(cmdList, params);

	computeCmdList.PIXEndEvent();
}

const Texture& FSR2Pass::GetOutput(SHADER_OUT_RES res) noexcept
{
	Assert((int)res < (int)SHADER_OUT_RES::COUNT, "out-of-bound access");
	return FSR2_Internal::GetUpscaledOutput();
}