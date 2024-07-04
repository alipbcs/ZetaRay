#include "CommandList.h"
#include "RendererCore.h"

using namespace ZetaRay::Core;

//--------------------------------------------------------------------------------------
// CommandContext
//--------------------------------------------------------------------------------------

CommandList::CommandList(D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* cmdAlloc)
    : m_cmdAllocator(cmdAlloc),
    m_type(t)
{
    auto* device = App::GetRenderer().GetDevice();

    ID3D12GraphicsCommandList* cmdList = nullptr;
    CheckHR(device->CreateCommandList(0, t, m_cmdAllocator, nullptr, IID_PPV_ARGS(&cmdList)));
    CheckHR(cmdList->QueryInterface(IID_PPV_ARGS(m_cmdList.GetAddressOf())));
    Assert(cmdList->Release() == 1, "bug");
}

void CommandList::Reset(ID3D12CommandAllocator* cmdAlloc)
{
    Assert(m_cmdList && !m_cmdAllocator, "bug");
    m_cmdAllocator = cmdAlloc;
    CheckHR(m_cmdList->Reset(m_cmdAllocator, nullptr));

    // D3D specs: "There is a new ordering constraint between SetDescriptorHeaps and 
    // SetGraphicsRootSignature or SetComputeRootSignature. SetDescriptorHeaps must be 
    // called, passing the corresponding heaps, before a call to SetGraphicsRootSignature 
    // or SetComputeRootSignature that uses either CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED or 
    // SAMPLER_HEAP_DIRECTLY_INDEXED flags."
    auto& renderer = App::GetRenderer();
    ID3D12DescriptorHeap* heaps[] = {
        renderer.GetGpuDescriptorHeap().GetHeap(),
        renderer.GetSamplerDescriptorHeap()
    };
    m_cmdList->SetDescriptorHeaps(ZetaArrayLen(heaps), heaps);
}
