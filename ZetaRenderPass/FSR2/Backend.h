#pragma once

#include "../RenderPass.h"
#include <FSR2/Include/ffx_fsr2.h>

namespace ZetaRay::Core::GpuMemory
{
	struct Texture;
}

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::RenderPass::FSR2_Internal
{
	struct DispatchParams
	{
		ID3D12Resource* DepthBuffer;
		ID3D12Resource* Color;
		ID3D12Resource* MotionVectors;
		ID3D12Resource* Exposure;
	};

	void Init(DXGI_FORMAT outputFormat, int outputWidth, int outputHeight);
	void Shutdown();
	bool IsInitialized();
	//void OnWindowResized(DXGI_FORMAT outputFormat, int outputWidth, int outputHeight);
	const Core::GpuMemory::Texture& GetUpscaledOutput();
	void Dispatch(Core::CommandList& cmdList, const DispatchParams& params);

	FfxErrorCode Fsr2CreateBackendContext(FfxFsr2Interface* backendInterface, FfxDevice device);
	FfxErrorCode Fsr2DestroyBackendContext(FfxFsr2Interface* backendInterface);
	FfxErrorCode Fsr2GetDeviceCapabilities(FfxFsr2Interface* backendInterface,
		FfxDeviceCapabilities* outDeviceCapabilities, FfxDevice device);
	FfxErrorCode Fsr2CreateResource(FfxFsr2Interface* backendInterface,
		const FfxCreateResourceDescription* resDesc, FfxResourceInternal* outResource);
	FfxErrorCode Fsr2RegisterResource(FfxFsr2Interface* backendInterface, const FfxResource* inResource,
		FfxResourceInternal* outResource);
	FfxErrorCode Fsr2UnregisterResources(FfxFsr2Interface* backendInterface);
	FfxResourceDescription Fsr2GetResourceDescription(FfxFsr2Interface* backendInterface, FfxResourceInternal resource);
	FfxErrorCode Fsr2DestroyResource(FfxFsr2Interface* backendInterface, FfxResourceInternal resource);
	FfxErrorCode Fsr2CreatePipeline(FfxFsr2Interface* backendInterface, FfxFsr2Pass pass,
		const FfxPipelineDescription* psoDesc, FfxPipelineState* outPipeline);
	FfxErrorCode Fsr2DestroyPipeline(FfxFsr2Interface* backendInterface, FfxPipelineState* pipeline);
	FfxErrorCode Fsr2ScheduleGpuJob(FfxFsr2Interface* backendInterface, const FfxGpuJobDescription* job);
	FfxErrorCode Fsr2ExecuteGpuJobs(FfxFsr2Interface* backendInterface, FfxCommandList commandList);
}
