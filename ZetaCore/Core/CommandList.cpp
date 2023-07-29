#include "CommandList.h"
#include "../App/App.h"
#include "RendererCore.h"

using namespace ZetaRay::Core;

//--------------------------------------------------------------------------------------
// CommandContext
//--------------------------------------------------------------------------------------

CommandList::CommandList(D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* cmdAlloc) noexcept
	: m_cmdAllocator(cmdAlloc),
	m_type(t)
{
	auto* device = App::GetRenderer().GetDevice();

	ID3D12GraphicsCommandList* cmdList = nullptr;
	CheckHR(device->CreateCommandList(0, t, m_cmdAllocator, nullptr, IID_PPV_ARGS(&cmdList)));
	CheckHR(cmdList->QueryInterface(IID_PPV_ARGS(m_cmdList.GetAddressOf())));
	Assert(cmdList->Release() == 1, "bug");
}

void CommandList::Reset(ID3D12CommandAllocator* cmdAlloc) noexcept
{
	Assert(m_cmdList && !m_cmdAllocator, "bug");
	m_cmdAllocator = cmdAlloc;
	CheckHR(m_cmdList->Reset(m_cmdAllocator, nullptr));

	ID3D12DescriptorHeap* gpuDescHeap = App::GetRenderer().GetGpuDescriptorHeap().GetHeap();
	m_cmdList->SetDescriptorHeaps(1, &gpuDescHeap);
}
