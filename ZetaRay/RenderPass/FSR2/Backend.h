#pragma once

#include "../RenderPass.h"
#include <FSR2/Include/ffx_fsr2.h>

namespace ZetaRay::Core
{
	struct Texture;
	class CommandList;
}

namespace ZetaRay::RenderPass::FSR2_Internal
{
	struct DispatchParams
	{
		ID3D12Resource* DepthBuffer;
		ID3D12Resource* Color;
		ID3D12Resource* MotionVectors;
	};

	void Init(DXGI_FORMAT outputFormat, int outputWidth, int outputHeight) noexcept;
	void Shutdown() noexcept;
	bool IsInitialized() noexcept;
	//void OnWindowResized(DXGI_FORMAT outputFormat, int outputWidth, int outputHeight) noexcept;
	const Core::Texture& GetUpscaledOutput() noexcept;
	void Dispatch(Core::CommandList& cmdList, const DispatchParams& params) noexcept;

	FfxErrorCode Fsr2CreateDeviceFunc(FfxFsr2Interface* backendInterface, FfxDevice outDevice);
	FfxErrorCode Fsr2GetDeviceCapabilities(FfxFsr2Interface* backendInterface,
		FfxDeviceCapabilities* outDeviceCapabilities, FfxDevice device);
	FfxErrorCode Fsr2DestroyDevice(FfxFsr2Interface* backendInterface, FfxDevice device);
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
	FfxErrorCode Fsr2ScheduleRenderJob(FfxFsr2Interface* backendInterface, const FfxRenderJobDescription* job);
	FfxErrorCode Fsr2ExecuteRenderJobs(FfxFsr2Interface* backendInterface, FfxCommandList commandList);
}
