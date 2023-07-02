#include "Backend.h"
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <App/Common.h>
#include <App/Timer.h>
#include <Scene/SceneCore.h>
#include <Support/Task.h>
#include <algorithm>
#include <Scene/Camera.h>
#include <Core/PipelineStateLibrary.h>

#include <FSR2/Include/shaders/ffx_fsr2_resources.h>
#include <FSR2/Include/dx12/shaders/ffx_fsr2_shaders_dx12.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

namespace
{
	struct ResourceData
	{
		D3D12_RESOURCE_STATES State;
		DescriptorTable SrvAllMipsCpu;
		DescriptorTable UavAllMipsCpu;
		DescriptorTable UavAllMipsGpu;
		bool NeedsUavBarrier = false;
		bool RecordedClearThisFrame = false;
	};

	struct RenderPassData
	{
		ComPtr<ID3D12RootSignature> RootSig;
		ID3D12PipelineState* PSO = nullptr;

		DescriptorTable SrvTableGpu;
		int SrvTableGpuNumDescs = -1;
		DescriptorTable UavTableGpu;
		int UavTableGpuNumDescs = -1;
	};

	struct PsoMap
	{
		ID3D12PipelineState* PSO;
		FfxFsr2Pass Pass;
	};

	struct DllWrapper
	{
		using fp_Fsr2ContextCreate = FfxErrorCode(*)(FfxFsr2Context* context, const FfxFsr2ContextDescription* contextDescription);
		using fp_Fsr2ContextDestroy = FfxErrorCode(*)(FfxFsr2Context* context);
		using fp_Fsr2ContextDispatch = FfxErrorCode (*)(FfxFsr2Context* context, const FfxFsr2DispatchDescription* dispatchDescription);
		using fp_Fsr2GetPermBlobByIdx = Fsr2ShaderBlobDX12 (*)(FfxFsr2Pass passId, uint32_t permutationOptions);

		void Load() noexcept
		{
			m_fsrLib = LoadLibraryExA("ffx_fsr2_api_x64", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
			CheckWin32(m_fsrLib);
			m_fsrDxLib = LoadLibraryExA("ffx_fsr2_api_dx12_x64", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
			CheckWin32(m_fsrDxLib);

			FpCreate = reinterpret_cast<fp_Fsr2ContextCreate>(GetProcAddress(m_fsrLib, "ffxFsr2ContextCreate"));
			CheckWin32(FpCreate);
			FpDestroy = reinterpret_cast<fp_Fsr2ContextDestroy>(GetProcAddress(m_fsrLib, "ffxFsr2ContextDestroy"));
			CheckWin32(FpDestroy);
			FpDispatch = reinterpret_cast<fp_Fsr2ContextDispatch>(GetProcAddress(m_fsrLib, "ffxFsr2ContextDispatch"));
			CheckWin32(FpDispatch);
			FpGetShaderPermutation = reinterpret_cast<fp_Fsr2GetPermBlobByIdx>(GetProcAddress(m_fsrDxLib, "fsr2GetPermutationBlobByIndexDX12"));
			CheckWin32(FpGetShaderPermutation);
		}

		void Free() noexcept
		{
			if(m_fsrLib)
				FreeLibrary(m_fsrLib);
			if(m_fsrDxLib)
				FreeLibrary(m_fsrDxLib);

			FpCreate = nullptr;
			FpDestroy = nullptr;
			FpDispatch = nullptr;
			FpGetShaderPermutation = nullptr;
		}

		HMODULE m_fsrLib;
		HMODULE m_fsrDxLib;

		fp_Fsr2ContextCreate FpCreate = nullptr;
		fp_Fsr2ContextDestroy FpDestroy = nullptr;
		fp_Fsr2ContextDispatch FpDispatch = nullptr;
		fp_Fsr2GetPermBlobByIdx FpGetShaderPermutation = nullptr;
	};

	//
	// Data
	//
	struct FSR2_Data
	{
		static constexpr uint32_t FLAGS = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE |
			FFX_FSR2_ENABLE_DEPTH_INVERTED |
			FFX_FSR2_ENABLE_DEPTH_INFINITE;

		static constexpr uint32_t APP_CONTROLLED_RES_IDS[] = 
		{ 
			FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_COLOR,
			FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS,
			FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_DEPTH,
			FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_EXPOSURE,
			FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT 
		};

		static constexpr uint32_t NUM_APP_CONTROLLED_RESOURCES = ZetaArrayLen(APP_CONTROLLED_RES_IDS);

		static constexpr int MAX_BARRIERS = 16;
		static constexpr int MAX_SAMPLERS = 2;
		static constexpr int MAX_DESC_RANGES = 2;
		static constexpr int MAX_ROOT_PARAMS = 10;
		static constexpr int MAX_NUM_CONST_BUFFERS = 2;

		FfxFsr2Context m_ctx;

		UploadHeapBuffer m_uploadHeapBuffs[FFX_FSR2_RESOURCE_IDENTIFIER_COUNT];
		DefaultHeapBuffer m_defaultHeapBuffs[FFX_FSR2_RESOURCE_IDENTIFIER_COUNT];
		Texture m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_COUNT];
		ResourceData m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_COUNT];

		bool m_reset = true;

		RenderPassData m_passes[FFX_FSR2_PASS_COUNT];
		PsoMap m_psoToPassMap[FFX_FSR2_PASS_COUNT];
		int m_currMapIdx = 0;

		ComputeCmdList* m_cmdList = nullptr;

		// app-controlled resources
		ID3D12Resource* m_color = nullptr;
		ID3D12Resource* m_depth = nullptr;
		ID3D12Resource* m_motionVec = nullptr;
		ID3D12Resource* m_exposure = nullptr;

		PipelineStateLibrary m_psoLib;
		DllWrapper m_dll;
	};

	FSR2_Data* g_fsr2Data = nullptr;

	D3D12_RESOURCE_STATES GetD3D12State(FfxResourceStates fsrState) noexcept
	{
		switch (fsrState)
		{
		case FFX_RESOURCE_STATE_UNORDERED_ACCESS:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case FFX_RESOURCE_STATE_COMPUTE_READ:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case FFX_RESOURCE_STATE_COPY_SRC:
			return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case FFX_RESOURCE_STATE_COPY_DEST:
			return D3D12_RESOURCE_STATE_COPY_DEST;
		case FFX_RESOURCE_STATE_GENERIC_READ:
			return D3D12_RESOURCE_STATE_GENERIC_READ;
		default:
			Assert(false, "Unknown state");
			return D3D12_RESOURCE_STATE_COMMON;
		}
	}

	const char* GetFsrErrorMsg(FfxErrorCode err) noexcept
	{
		if (err == FFX_ERROR_INVALID_POINTER)
			return "The operation failed due to an invalid pointer";
		else if (err == FFX_ERROR_INVALID_ALIGNMENT)
			return "The operation failed due to an invalid alignment.";
		else if (err == FFX_ERROR_INVALID_SIZE)
			return "The operation failed due to an invalid size.";
		else if (err == FFX_EOF)
			return "The end of the file was encountered.";
		else if (err == FFX_ERROR_INVALID_PATH)
			return "The operation failed because the specified path was invalid.";
		else if (err == FFX_ERROR_EOF)
			return "The operation failed because end of file was reached.";
		else if (err == FFX_ERROR_MALFORMED_DATA)
			return "The operation failed because of some malformed data.";
		else if (err == FFX_ERROR_OUT_OF_MEMORY)
			return "The operation failed because it ran out memory.";
		else if (err == FFX_ERROR_INCOMPLETE_INTERFACE)
			return "The operation failed because the interface was not fully configured.";
		else if (err == FFX_ERROR_INVALID_ENUM)
			return "The operation failed because of an invalid enumeration value.";
		else if (err == FFX_ERROR_INVALID_ARGUMENT)
			return "The operation failed because an argument was invalid.";
		else if (err == FFX_ERROR_OUT_OF_RANGE)
			return "The operation failed because a value was out of range.";
		else if (err == FFX_ERROR_NULL_DEVICE)
			return "The operation failed because a device was null.";
		else if (err == FFX_ERROR_BACKEND_API_ERROR)
			return "The operation failed because the backend API returned an error code.";
		else if (err == FFX_ERROR_INSUFFICIENT_MEMORY)
			return "The operation failed because there was not enough memory.";

		return "Unknown error.";
	}

	DXGI_FORMAT ToDXGIFormat(FfxSurfaceFormat surfaceFormat)
	{
		switch (surfaceFormat)
		{
		case(FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS):
			return DXGI_FORMAT_R32G32B32A32_TYPELESS;
		case(FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT):
			return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case(FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT):
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case(FFX_SURFACE_FORMAT_R16G16B16A16_UNORM):
			return DXGI_FORMAT_R16G16B16A16_UNORM;
		case(FFX_SURFACE_FORMAT_R32G32_FLOAT):
			return DXGI_FORMAT_R32G32_FLOAT;
		case(FFX_SURFACE_FORMAT_R32_UINT):
			return DXGI_FORMAT_R32_UINT;
		case(FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS):
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM):
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case(FFX_SURFACE_FORMAT_R11G11B10_FLOAT):
			return DXGI_FORMAT_R11G11B10_FLOAT;
		case(FFX_SURFACE_FORMAT_R16G16_FLOAT):
			return DXGI_FORMAT_R16G16_FLOAT;
		case(FFX_SURFACE_FORMAT_R16G16_UINT):
			return DXGI_FORMAT_R16G16_UINT;
		case(FFX_SURFACE_FORMAT_R16_FLOAT):
			return DXGI_FORMAT_R16_FLOAT;
		case(FFX_SURFACE_FORMAT_R16_UINT):
			return DXGI_FORMAT_R16_UINT;
		case(FFX_SURFACE_FORMAT_R16_UNORM):
			return DXGI_FORMAT_R16_UNORM;
		case(FFX_SURFACE_FORMAT_R16_SNORM):
			return DXGI_FORMAT_R16_SNORM;
		case(FFX_SURFACE_FORMAT_R8_UNORM):
			return DXGI_FORMAT_R8_UNORM;
		case(FFX_SURFACE_FORMAT_R8_UINT):
			return DXGI_FORMAT_R8_UINT;
		case(FFX_SURFACE_FORMAT_R8G8_UNORM):
			return DXGI_FORMAT_R8G8_UNORM;
		case(FFX_SURFACE_FORMAT_R32_FLOAT):
			return DXGI_FORMAT_R32_FLOAT;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	int FindPSO(ID3D12PipelineState* key) noexcept
	{
		int beg = 0;
		int end = FFX_FSR2_PASS_COUNT;
		int mid = end >> 1;

		while (true)
		{
			if (end - beg <= 2)
				break;

			if (g_fsr2Data->m_psoToPassMap[mid].PSO < key)
				beg = mid + 1;
			else
				end = mid + 1;

			mid = beg + ((end - beg) >> 1);
		}

		if (g_fsr2Data->m_psoToPassMap[beg].PSO == key)
			return beg;
		else if (g_fsr2Data->m_psoToPassMap[mid].PSO == key)
			return mid;

		return -1;
	}

	void RecordClearJob(const FfxClearFloatJobDescription& job) noexcept
	{
		Assert(g_fsr2Data->m_cmdList, "Command list was NULL");
		Assert(job.target.internalIndex < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT, "Unknown resource");

		for (uint32_t i = 0; i < g_fsr2Data->NUM_APP_CONTROLLED_RESOURCES; i++)
			Assert(job.target.internalIndex != (int)g_fsr2Data->APP_CONTROLLED_RES_IDS[i], "This resource is controlled by the App.");

		if (g_fsr2Data->m_resData[job.target.internalIndex].RecordedClearThisFrame)
			return;

		Texture& t = g_fsr2Data->m_textures[job.target.internalIndex];
		Assert(t.IsInitialized(), "Texture hasn't been created yet.");

		auto& uavDescTableCpu = g_fsr2Data->m_resData[job.target.internalIndex].UavAllMipsCpu;
		auto& uavDescTableGpu = g_fsr2Data->m_resData[job.target.internalIndex].UavAllMipsGpu;

		const auto desc = t.GetResource()->GetDesc();
		Assert(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "UAV access is not allowed for this resource");

		if (uavDescTableCpu.IsEmpty())
		{
			uavDescTableCpu = App::GetRenderer().GetCbvSrvUavDescriptorHeapCpu().Allocate(desc.MipLevels);

			for (uint32_t i = 0; i < desc.MipLevels; i++)
				Direct3DHelper::CreateTexture2DUAV(t, uavDescTableCpu.CPUHandle(i), DXGI_FORMAT_UNKNOWN, i);
		}

		if (uavDescTableGpu.IsEmpty())
		{
			uavDescTableGpu = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(desc.MipLevels);

			for (uint32_t i = 0; i < desc.MipLevels; i++)
				Direct3DHelper::CreateTexture2DUAV(t, uavDescTableGpu.CPUHandle(i), DXGI_FORMAT_UNKNOWN, i);
		}

		if (job.target.internalIndex == FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT)
		{
			Assert(g_fsr2Data->m_resData[job.target.internalIndex].State == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				"upscaled color should always be in UAV state");
		}
		else if (g_fsr2Data->m_resData[job.target.internalIndex].State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		{
			auto barrier = Direct3DHelper::TransitionBarrier(t.GetResource(), g_fsr2Data->m_resData[job.target.internalIndex].State,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// TODO barriers should be batched
			g_fsr2Data->m_cmdList->ResourceBarrier(&barrier, 1);

			g_fsr2Data->m_resData[job.target.internalIndex].State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		}

		g_fsr2Data->m_cmdList->ClearUnorderedAccessViewFloat(uavDescTableGpu.GPUHandle(0), 
			uavDescTableCpu.CPUHandle(0),
			t.GetResource(), 
			job.color[0], job.color[1], job.color[2], job.color[3]);

		g_fsr2Data->m_resData[job.target.internalIndex].NeedsUavBarrier = true;
		g_fsr2Data->m_resData[job.target.internalIndex].RecordedClearThisFrame = true;
	}	
	
	void RecordComputeJob(const FfxComputeJobDescription& job) noexcept
	{
		Assert(g_fsr2Data->m_cmdList, "Command list was NULL");

		g_fsr2Data->m_cmdList->SetRootSignature(reinterpret_cast<ID3D12RootSignature*>(job.pipeline.rootSignature));
		g_fsr2Data->m_cmdList->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(job.pipeline.pipeline));

		auto& renderer = App::GetRenderer();
		auto* device = renderer.GetDevice();
		const int idx = FindPSO(reinterpret_cast<ID3D12PipelineState*>(job.pipeline.pipeline));
		Assert(idx != -1, "Given PSO was not found");

		const FfxFsr2Pass pass = g_fsr2Data->m_psoToPassMap[idx].Pass;
		Assert(pass < FFX_FSR2_PASS_COUNT, "Invalid pass");

		auto& passData = g_fsr2Data->m_passes[pass];
		
		D3D12_RESOURCE_BARRIER barriers[g_fsr2Data->MAX_BARRIERS];
		int currBarrierIdx = 0;

		g_fsr2Data->m_passes[pass].SrvTableGpu = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate(
			g_fsr2Data->m_passes[pass].SrvTableGpuNumDescs);

		g_fsr2Data->m_passes[pass].UavTableGpu = renderer.GetCbvSrvUavDescriptorHeapGpu().Allocate(
			g_fsr2Data->m_passes[pass].UavTableGpuNumDescs);

		// UAVs
		for (uint32_t i = 0; i < job.pipeline.uavCount; i++)
		{
			const uint32_t uavResIdx = job.uavs[i].internalIndex;
			Assert(uavResIdx < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT, "Unknown resource");
			//Assert(job.pipeline.uavResourceBindings[i].resourceIdentifier == uavResIdx + job.uavMip[i], "Unexpected mismatch");

			auto& uavAllMips = g_fsr2Data->m_resData[uavResIdx].UavAllMipsCpu;
			
			{
				Texture& t = g_fsr2Data->m_textures[uavResIdx];
				Assert(t.GetResource(), "Texture2D hasn't been created yet.");

				const auto desc = t.GetResource()->GetDesc();
				Assert(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "UAV access is not allowed for this resource");

				if (uavAllMips.IsEmpty())
					uavAllMips = App::GetRenderer().GetCbvSrvUavDescriptorHeapCpu().Allocate(desc.MipLevels);

				for (uint32_t j = 0; j < desc.MipLevels; j++)
					Direct3DHelper::CreateTexture2DUAV(t, uavAllMips.CPUHandle(j), DXGI_FORMAT_UNKNOWN, j);
			}

			if ((g_fsr2Data->m_resData[uavResIdx].State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0)
			{
				Texture& t = g_fsr2Data->m_textures[uavResIdx];

				barriers[currBarrierIdx++] = Direct3DHelper::TransitionBarrier(t.GetResource(),
					g_fsr2Data->m_resData[uavResIdx].State,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				g_fsr2Data->m_resData[uavResIdx].State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			// TODO only necessary if there's been a clear operation on this resource in this frame
			else if(g_fsr2Data->m_resData[uavResIdx].NeedsUavBarrier)
			{
				Texture& t = g_fsr2Data->m_textures[uavResIdx];
				barriers[currBarrierIdx++] = Direct3DHelper::UAVBarrier(t.GetResource());

				g_fsr2Data->m_resData[uavResIdx].NeedsUavBarrier = false;
			}

			const int uavBindSlot = job.pipeline.uavResourceBindings[i].slotIndex;

			device->CopyDescriptorsSimple(1,
				passData.UavTableGpu.CPUHandle(uavBindSlot),
				uavAllMips.CPUHandle(job.uavMip[i]),
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		uint32_t currRootParam = 0;
		g_fsr2Data->m_cmdList->SetRootDescriptorTable(currRootParam++, passData.UavTableGpu.GPUHandle(0));
		
		// SRVs
		for (uint32_t i = 0; i < job.pipeline.srvCount; i++)
		{
			const uint32_t srvResIdx = job.srvs[i].internalIndex;
			Assert(srvResIdx < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT, "Unknown resource");
			//Assert(job.pipeline.srvResourceBindings[i].resourceIdentifier == srvResIdx, "Unexpected mismatch");

			auto& srv = g_fsr2Data->m_resData[srvResIdx].SrvAllMipsCpu;
			if (srv.IsEmpty())
			{
				srv = App::GetRenderer().GetCbvSrvUavDescriptorHeapCpu().Allocate(1);

				Texture& t = g_fsr2Data->m_textures[srvResIdx];
				Assert(t.IsInitialized(), "Texture2D hasn't been created yet.");
				Direct3DHelper::CreateTexture2DSRV(t, srv.CPUHandle(0));
			}

			if ((g_fsr2Data->m_resData[srvResIdx].State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
			{
				Texture& t = g_fsr2Data->m_textures[srvResIdx];

				barriers[currBarrierIdx++] = Direct3DHelper::TransitionBarrier(t.GetResource(),
					g_fsr2Data->m_resData[srvResIdx].State,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

				g_fsr2Data->m_resData[srvResIdx].State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}

			const int srvBindSlot = job.pipeline.srvResourceBindings[i].slotIndex;

			device->CopyDescriptorsSimple(1,
				passData.SrvTableGpu.CPUHandle(srvBindSlot),
				srv.CPUHandle(0),
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
		
		g_fsr2Data->m_cmdList->SetRootDescriptorTable(currRootParam++, passData.SrvTableGpu.GPUHandle(0));

		// root constants
		for (uint32_t currRootConstantIdx = 0; currRootConstantIdx < job.pipeline.constCount; ++currRootConstantIdx)
		{
			g_fsr2Data->m_cmdList->SetRoot32BitConstants(currRootParam + currRootConstantIdx,
				job.cbs[currRootConstantIdx].uint32Size,
				job.cbs[currRootConstantIdx].data, 
				0);
		}

		if(currBarrierIdx)
			g_fsr2Data->m_cmdList->ResourceBarrier(barriers, currBarrierIdx);

		g_fsr2Data->m_cmdList->Dispatch(job.dimensions[0], job.dimensions[1], job.dimensions[2]);
	}

	/*
	void LogMsg(FfxFsr2MsgType type, const wchar_t* message)
	{
		char msg[512];
		Common::WideToCharStr(message, msg);
		Assert(false, "FSR2 [%s]: %s", type == FFX_FSR2_MESSAGE_TYPE_ERROR ? "Error" : "Warning", msg);
	}
	*/
}

#ifdef _DEBUG 
#ifndef CheckFSR
#define CheckFSR(x)											                															\
    {																    																\
	    FfxErrorCode err = (x);											    															\
	    if (err != FFX_OK)												    															\
	    {															           															\
            char buff[256];																												\
            stbsp_snprintf(buff, 256, "%s: %d\nFSR call %s\n failed with error:\n%s", __FILE__,  __LINE__, #x, GetFsrErrorMsg(err));	\
			MessageBoxA(nullptr, buff, "Fatal Error", MB_ICONERROR | MB_OK);															\
		    __debugbreak();																												\
	    }																																\
    }
#endif
#else
#ifndef CheckFSR
#define CheckFSR(x)																														\
    {																																	\
	    FfxErrorCode err = (x);																											\
	    if (err != FFX_OK)																												\
	    {																																\
            char buff[256];																												\
            stbsp_snprintf(buff, 256, "%s: %d\nFSR call %s\n failed with error:\n%s", __FILE__,  __LINE__, #x, GetFsrErrorMsg(err));	\
			MessageBoxA(nullptr, buff, "Fatal Error", MB_ICONERROR | MB_OK);															\
            exit(EXIT_FAILURE);																											\
	    }																																\
    }
#endif // DEBUGBREAK
#endif

//--------------------------------------------------------------------------------------
// FSR2_Internal
//--------------------------------------------------------------------------------------

void FSR2_Internal::Init(DXGI_FORMAT outputFormat, int outputWidth, int outputHeight) noexcept
{
	if(!g_fsr2Data)
		g_fsr2Data = new FSR2_Data;

	FfxFsr2Interface fsr2Interface;
	fsr2Interface.fpCreateBackendContext = FSR2_Internal::Fsr2CreateBackendContext;
	fsr2Interface.fpGetDeviceCapabilities = FSR2_Internal::Fsr2GetDeviceCapabilities;
	fsr2Interface.fpDestroyBackendContext = FSR2_Internal::Fsr2DestroyBackendContext;
	fsr2Interface.fpCreateResource = FSR2_Internal::Fsr2CreateResource;
	fsr2Interface.fpRegisterResource = FSR2_Internal::Fsr2RegisterResource;
	fsr2Interface.fpUnregisterResources = FSR2_Internal::Fsr2UnregisterResources;
	fsr2Interface.fpGetResourceDescription = FSR2_Internal::Fsr2GetResourceDescription;
	fsr2Interface.fpDestroyResource = FSR2_Internal::Fsr2DestroyResource;
	fsr2Interface.fpCreatePipeline = FSR2_Internal::Fsr2CreatePipeline;
	fsr2Interface.fpDestroyPipeline = FSR2_Internal::Fsr2DestroyPipeline;
	fsr2Interface.fpScheduleGpuJob = FSR2_Internal::Fsr2ScheduleGpuJob;
	fsr2Interface.fpExecuteGpuJobs = FSR2_Internal::Fsr2ExecuteGpuJobs;
	fsr2Interface.scratchBuffer = nullptr;
	fsr2Interface.scratchBufferSize = 0;
	
	auto& renderer = App::GetRenderer();

	FfxFsr2ContextDescription ctxDesc;
	ctxDesc.flags = g_fsr2Data->FLAGS;
	ctxDesc.maxRenderSize.width = renderer.GetRenderWidth();
	ctxDesc.maxRenderSize.height = renderer.GetRenderHeight();
	ctxDesc.displaySize.width = renderer.GetDisplayWidth();
	ctxDesc.displaySize.height = renderer.GetDisplayHeight();
	ctxDesc.callbacks = fsr2Interface;
	ctxDesc.device = App::GetRenderer().GetDevice();
	//ctxDesc.fpMessage = LogMsg;
	ctxDesc.fpMessage = nullptr;

	memset(g_fsr2Data->m_psoToPassMap, 0, sizeof(PsoMap) * FFX_FSR2_PASS_COUNT);

	// initialize the PSO library (must be called before ffxFsr2ContextCreate)
	g_fsr2Data->m_psoLib.Init("FSR2");

	g_fsr2Data->m_dll.Load();
	CheckFSR(g_fsr2Data->m_dll.FpCreate(&g_fsr2Data->m_ctx, &ctxDesc));

	// upscaled output texture
	Assert(!g_fsr2Data->m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].IsInitialized(), "Output is app-controlled");
	g_fsr2Data->m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT] = renderer.GetGpuMemory().GetTexture2D("UpscaledColor",
		outputWidth,
		outputHeight,
		outputFormat,
		D3D12_RESOURCE_STATE_COMMON, 
		TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS);

	// render graph performs the transition to UAV prior to recording
	g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	Assert(g_fsr2Data->m_currMapIdx == FFX_FSR2_PASS_COUNT, "Unaccounted PSOs");
	std::sort(g_fsr2Data->m_psoToPassMap, g_fsr2Data->m_psoToPassMap + FFX_FSR2_PASS_COUNT,
		[](const PsoMap& p1, const PsoMap& p2)
		{
			return p1.PSO < p2.PSO;
		});
}

void FSR2_Internal::Shutdown() noexcept
{
	if (g_fsr2Data)
	{
		//CheckFSR(ffxFsr2ContextDestroy(&g_fsr2Data->m_ctx));
		CheckFSR(g_fsr2Data->m_dll.FpDestroy(&g_fsr2Data->m_ctx));
		g_fsr2Data->m_dll.Free();

		/*
		for (int i = 0; i < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT; i++)
		{
			// clear the UAV barrier flags
			g_fsr2Data->m_resData[i].NeedsUavBarrier = false;

			// proper cpu-gpu sync. is done automatically
			if (!g_fsr2Data->m_resData[i].UavAllMipsGpu.IsEmpty())
				g_fsr2Data->m_resData[i].UavAllMipsGpu.Reset();
		}
		*/

		// make sure GPU is finished with related resources before deleting the data
		Task t("DestructWithGuard for FSR2 context", TASK_PRIORITY::BACKGRUND, [res = g_fsr2Data]()
			{
				ComPtr<ID3D12Fence> fenceDirect;
				ComPtr<ID3D12Fence> fenceCompute;

				auto* device = App::GetRenderer().GetDevice();
				CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fenceDirect.GetAddressOf())));
				CheckHR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fenceCompute.GetAddressOf())));

				App::GetRenderer().SignalComputeQueue(fenceCompute.Get(), 1);
				App::GetRenderer().SignalDirectQueue(fenceDirect.Get(), 1);

				HANDLE handleCompute = CreateEventA(nullptr, false, false, "");
				CheckWin32(handleCompute);
				HANDLE handleDirect = CreateEventA(nullptr, false, false, "");
				CheckWin32(handleDirect);

				CheckHR(fenceCompute->SetEventOnCompletion(1, handleCompute));
				CheckHR(fenceDirect->SetEventOnCompletion(1, handleDirect));

				HANDLE handles[] = { handleCompute, handleDirect };

				WaitForMultipleObjects(2, handles, true, INFINITE);
				CloseHandle(handleDirect);
				CloseHandle(handleCompute);
				
				delete res;
			});

		// submit
		App::SubmitBackground(ZetaMove(t));

		g_fsr2Data = nullptr;
	}
}

bool FSR2_Internal::IsInitialized() noexcept
{
	return g_fsr2Data != nullptr;
}

const Texture& FSR2_Internal::GetUpscaledOutput() noexcept
{
	Assert(g_fsr2Data, "g_fsr2Data is NULL");
	Assert(g_fsr2Data->m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].IsInitialized(), "Texture hasn't been initialized.");
	return g_fsr2Data->m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT];
}

void FSR2_Internal::Dispatch(CommandList& cmdList, const DispatchParams& appParams) noexcept
{
	g_fsr2Data->m_cmdList = &static_cast<ComputeCmdList&>(cmdList);
	g_fsr2Data->m_color = appParams.Color;
	g_fsr2Data->m_depth = appParams.DepthBuffer;
	g_fsr2Data->m_motionVec = appParams.MotionVectors;
	g_fsr2Data->m_exposure = appParams.Exposure;

	auto func = [](ID3D12Resource* res, DescriptorTable& descTable)
	{
		auto* device = App::GetRenderer().GetDevice();
		auto desc = res->GetDesc();

		//if(descTable.IsEmpty())
			descTable = App::GetRenderer().GetCbvSrvUavDescriptorHeapCpu().Allocate(1);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.PlaneSlice = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Format = desc.Format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : desc.Format;

		device->CreateShaderResourceView(res, &srvDesc, descTable.CPUHandle(0));
	};

	func(g_fsr2Data->m_color, g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_COLOR].SrvAllMipsCpu);
	func(g_fsr2Data->m_depth, g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_DEPTH].SrvAllMipsCpu);
	func(g_fsr2Data->m_motionVec, g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS].SrvAllMipsCpu);
	func(g_fsr2Data->m_exposure, g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_EXPOSURE].SrvAllMipsCpu);

	g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_COLOR].State = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_DEPTH].State = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS].State = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_EXPOSURE].State = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	// render graph performs the transition to UAV prior to recording
	g_fsr2Data->m_resData[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	const auto& camera = App::GetCamera();

	FfxFsr2DispatchDescription params{};
	params.color.resource = appParams.Color;
	params.color.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	params.depth.resource = appParams.DepthBuffer;
	params.depth.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	params.depth.isDepth = true;
	params.motionVectors.resource = appParams.MotionVectors;
	params.motionVectors.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	params.output.resource = g_fsr2Data->m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].GetResource();
	params.output.state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
	params.exposure.resource = appParams.Exposure;
	params.exposure.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	params.jitterOffset.x = camera.GetCurrJitter().x;
	params.jitterOffset.y = camera.GetCurrJitter().y;
	params.cameraNear = FLT_MAX;
	params.cameraFar = camera.GetNearZ();
	params.cameraFovAngleVertical = camera.GetFOV();
	params.motionVectorScale.x = -(float)App::GetRenderer().GetRenderWidth();
	params.motionVectorScale.y = -(float)App::GetRenderer().GetRenderHeight();
	params.reset = g_fsr2Data->m_reset;
	params.enableSharpening = false;
	params.sharpness = 0.0f;
	params.frameTimeDelta = (float)(App::GetTimer().GetElapsedTime() * 1000);
	params.preExposure = 1.0f;
	params.renderSize.width = App::GetRenderer().GetRenderWidth();
	params.renderSize.height = App::GetRenderer().GetRenderHeight();
	params.viewSpaceToMetersFactor = 1.0f;

	g_fsr2Data->m_reset = false;

	auto& gpuTimer = App::GetRenderer().GetGpuTimer();
	
	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(*g_fsr2Data->m_cmdList, "FSR2");

	//CheckFSR(ffxFsr2ContextDispatch(&g_fsr2Data->m_ctx, &params));
	CheckFSR(g_fsr2Data->m_dll.FpDispatch(&g_fsr2Data->m_ctx, &params));

	// record the timestamp after execution
	gpuTimer.EndQuery(*g_fsr2Data->m_cmdList, queryIdx);

	g_fsr2Data->m_cmdList = nullptr;

	for (int i = 0; i < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT; i++)
	{
		// clear the UAV barrier flags
		g_fsr2Data->m_resData[i].NeedsUavBarrier = false;

		// proper cpu-gpu sync. is done automatically
		if (!g_fsr2Data->m_resData[i].UavAllMipsGpu.IsEmpty())
			g_fsr2Data->m_resData[i].UavAllMipsGpu.Reset();

		g_fsr2Data->m_resData[i].RecordedClearThisFrame = false;
	}
}

FfxErrorCode FSR2_Internal::Fsr2CreateBackendContext(FfxFsr2Interface* backendInterface, FfxDevice device)
{
	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2DestroyBackendContext(FfxFsr2Interface* backendInterface)
{
	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2GetDeviceCapabilities(FfxFsr2Interface* backendInterface,
	FfxDeviceCapabilities* outDeviceCapabilities, FfxDevice device)
{
	// support for following three was checked during app init
	outDeviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_6_6;
	outDeviceCapabilities->raytracingSupported = true;
	outDeviceCapabilities->fp16Supported = true;

	auto* d3dDevice = App::GetRenderer().GetDevice();

	// lane counts
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1;
	CheckHR(d3dDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));

	outDeviceCapabilities->waveLaneCountMin = options1.WaveLaneCountMin;
	outDeviceCapabilities->waveLaneCountMax = options1.WaveLaneCountMax;

	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2CreateResource(FfxFsr2Interface* backendInterface,
	const FfxCreateResourceDescription* resDesc, FfxResourceInternal* outResource)
{
	Assert(resDesc->id < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT, "invalid resource ID");

	for (uint32_t i = 0; i < g_fsr2Data->NUM_APP_CONTROLLED_RESOURCES; i++)
		Assert(resDesc->id != g_fsr2Data->APP_CONTROLLED_RES_IDS[i], "This resource is created by the App.");
	
	auto& gpuMem = App::GetRenderer().GetGpuMemory();

	// upload buffer
	if (resDesc->heapType == FFX_HEAP_TYPE_UPLOAD)
	{
		Assert(resDesc->initalState == FFX_RESOURCE_STATE_GENERIC_READ,
			"Upload heap buffer must be GENERIC_READ at all times");

		auto buff = gpuMem.GetUploadHeapBuffer(resDesc->initDataSize);
		buff.Copy(0, resDesc->initDataSize, resDesc->initData);

		g_fsr2Data->m_uploadHeapBuffs[resDesc->id] = ZetaMove(buff);
		outResource->internalIndex = resDesc->id;

		g_fsr2Data->m_resData[resDesc->id].State = D3D12_RESOURCE_STATE_GENERIC_READ;

		return FFX_OK;
	}
	
	// committed resource
	char resName[64];
	Common::WideToCharStr(resDesc->name, resName);

	const bool allowUAV = resDesc->usage & FFX_RESOURCE_USAGE_UAV;
	const bool allowRT = resDesc->usage & FFX_RESOURCE_USAGE_RENDERTARGET;
	const D3D12_RESOURCE_STATES state = GetD3D12State(resDesc->initalState);
	uint8_t textureFlags = 0;
	textureFlags = allowUAV ? textureFlags | TEXTURE_FLAGS::ALLOW_UNORDERED_ACCESS : textureFlags;
	textureFlags = allowRT ? textureFlags | TEXTURE_FLAGS::ALLOW_RENDER_TARGET : textureFlags;
	//const DXGI_FORMAT fmt = GetDXGIFormat(resDesc->resourceDescription.format);

	if (resDesc->resourceDescription.type == FFX_RESOURCE_TYPE_BUFFER)
	{
		Assert(resDesc->usage != FFX_RESOURCE_USAGE_RENDERTARGET, "Buffers can't be used as render targets.");

		if (resDesc->initData)
			g_fsr2Data->m_defaultHeapBuffs[resDesc->id] = gpuMem.GetDefaultHeapBufferAndInit(resName, resDesc->initDataSize, 
				state, allowUAV, resDesc->initData);
		else
			g_fsr2Data->m_defaultHeapBuffs[resDesc->id] = gpuMem.GetDefaultHeapBuffer(resName, resDesc->initDataSize, 
				state, allowUAV);

		outResource->internalIndex = resDesc->id;
	}
	else if (resDesc->resourceDescription.type == FFX_RESOURCE_TYPE_TEXTURE2D)
	{
		const DXGI_FORMAT fmt = ToDXGIFormat(resDesc->resourceDescription.format);
		Assert(fmt != DXGI_FORMAT_UNKNOWN, "Invalid Texture2D format.");

		if (resDesc->initData)
		{
			g_fsr2Data->m_textures[resDesc->id] = gpuMem.GetTexture2DAndInit(resName, 
				resDesc->resourceDescription.width,
				resDesc->resourceDescription.height,
				fmt,
				state,
				reinterpret_cast<uint8_t*>(resDesc->initData), 
				textureFlags);
		}
		else
		{
			g_fsr2Data->m_textures[resDesc->id] = gpuMem.GetTexture2D(resName,
				resDesc->resourceDescription.width,
				resDesc->resourceDescription.height,
				fmt,
				state,
				textureFlags,
				(uint16_t)resDesc->resourceDescription.mipCount);
		}

		outResource->internalIndex = resDesc->id;
	}
	else if (resDesc->resourceDescription.type == FFX_RESOURCE_TYPE_TEXTURE3D)
	{
		const DXGI_FORMAT fmt = ToDXGIFormat(resDesc->resourceDescription.format);
		Assert(!resDesc->initData, "Initializing Texture3D from CPU side is not supported.");
		Assert(fmt != DXGI_FORMAT_UNKNOWN, "Invalid Texture2D format.");

		g_fsr2Data->m_textures[resDesc->id] = gpuMem.GetTexture3D(resName,
			resDesc->resourceDescription.width,
			resDesc->resourceDescription.height,
			(uint16_t)resDesc->resourceDescription.depth,
			fmt,
			state,
			textureFlags,
			(uint16_t)resDesc->resourceDescription.mipCount);

		outResource->internalIndex = resDesc->id;
	}

	g_fsr2Data->m_resData[resDesc->id].State = state;

	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2RegisterResource(FfxFsr2Interface* backendInterface, const FfxResource* inResource,
	FfxResourceInternal* outResource)
{
	if (inResource->resource == nullptr)
		outResource->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_NULL;
	else if (reinterpret_cast<const ID3D12Resource*>(inResource->resource) == g_fsr2Data->m_color)
		outResource->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_COLOR;
	else if (reinterpret_cast<const ID3D12Resource*>(inResource->resource) == g_fsr2Data->m_depth)
		outResource->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_DEPTH;
	else if (reinterpret_cast<const ID3D12Resource*>(inResource->resource) == g_fsr2Data->m_motionVec)
		outResource->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS;
	else if (reinterpret_cast<const ID3D12Resource*>(inResource->resource) == g_fsr2Data->m_exposure)
		outResource->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_EXPOSURE;
	else if (reinterpret_cast<const ID3D12Resource*>(inResource->resource) == g_fsr2Data->m_textures[FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].GetResource())
		outResource->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT;

	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2UnregisterResources(FfxFsr2Interface* backendInterface)
{
	return FFX_OK;
}

FfxResourceDescription FSR2_Internal::Fsr2GetResourceDescription(FfxFsr2Interface* backendInterface,
	FfxResourceInternal resource)
{
	Assert(resource.internalIndex < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT, "Unknown resource idx");

	FfxResourceDescription ret{};

	if (resource.internalIndex == FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_COLOR)
	{
		Assert(g_fsr2Data->m_color, "Color input hasn't been set.");
		auto desc = g_fsr2Data->m_color->GetDesc();

		ret.type = FFX_RESOURCE_TYPE_TEXTURE2D;
		ret.mipCount = desc.MipLevels;
		ret.width = (uint32_t)desc.Width;
		ret.height = (uint32_t)desc.Height;
		ret.depth = desc.DepthOrArraySize;
		ret.flags = FFX_RESOURCE_FLAGS_NONE;

		return ret;
	}
	else if (resource.internalIndex == FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_DEPTH)
	{
		Assert(g_fsr2Data->m_depth, "Depth buffer input hasn't been set.");
		auto desc = g_fsr2Data->m_depth->GetDesc();

		ret.type = FFX_RESOURCE_TYPE_TEXTURE2D;
		ret.mipCount = desc.MipLevels;
		ret.width = (uint32_t)desc.Width;
		ret.height = (uint32_t)desc.Height;
		ret.depth = desc.DepthOrArraySize;
		ret.flags = FFX_RESOURCE_FLAGS_NONE;

		return ret;
	}	
	else if (resource.internalIndex == FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS)
	{
		Assert(g_fsr2Data->m_motionVec, "Motion vector input hasn't been set.");
		auto desc = g_fsr2Data->m_motionVec->GetDesc();

		ret.type = FFX_RESOURCE_TYPE_TEXTURE2D;
		ret.mipCount = desc.MipLevels;
		ret.width = (uint32_t)desc.Width;
		ret.height = (uint32_t)desc.Height;
		ret.depth = desc.DepthOrArraySize;
		ret.flags = FFX_RESOURCE_FLAGS_NONE;

		return ret;
	}	
	else if (resource.internalIndex == FFX_FSR2_RESOURCE_IDENTIFIER_INPUT_EXPOSURE)
	{
		Assert(g_fsr2Data->m_exposure, "Exposure input hasn't been set.");
		auto desc = g_fsr2Data->m_exposure->GetDesc();

		ret.type = FFX_RESOURCE_TYPE_TEXTURE2D;
		ret.mipCount = desc.MipLevels;
		ret.width = (uint32_t)desc.Width;
		ret.height = (uint32_t)desc.Height;
		ret.depth = desc.DepthOrArraySize;
		ret.flags = FFX_RESOURCE_FLAGS_NONE;

		return ret;
	}

	if (g_fsr2Data->m_textures[resource.internalIndex].IsInitialized())
	{
		auto desc = g_fsr2Data->m_textures[resource.internalIndex].GetDesc();

		ret.type = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? FFX_RESOURCE_TYPE_TEXTURE2D : FFX_RESOURCE_TYPE_TEXTURE3D;
		ret.mipCount = desc.MipLevels;
		ret.width = (uint32_t)desc.Width;
		ret.height = (uint32_t)desc.Height;
		ret.depth = desc.DepthOrArraySize;
		ret.flags = FFX_RESOURCE_FLAGS_NONE;
	}
	else if (g_fsr2Data->m_defaultHeapBuffs[resource.internalIndex].IsInitialized())
	{
		auto desc = g_fsr2Data->m_defaultHeapBuffs[resource.internalIndex].GetDesc();

		ret.type = FFX_RESOURCE_TYPE_BUFFER;
		ret.mipCount = desc.MipLevels;
		ret.width = (uint32_t)desc.Width;
		ret.height = (uint32_t)desc.Height;
		ret.depth = desc.DepthOrArraySize;
		ret.flags = FFX_RESOURCE_FLAGS_NONE;
	}
	else
		Assert(false, "Resource not found.");

	return ret;
}

FfxErrorCode FSR2_Internal::Fsr2DestroyResource(FfxFsr2Interface* backendInterface, FfxResourceInternal resource)
{
	Assert(resource.internalIndex < FFX_FSR2_RESOURCE_IDENTIFIER_COUNT, "Unknown resource idx");

	if(g_fsr2Data->m_textures[resource.internalIndex].IsInitialized())
		g_fsr2Data->m_textures[resource.internalIndex].Reset();
	else if (g_fsr2Data->m_defaultHeapBuffs[resource.internalIndex].IsInitialized())
		g_fsr2Data->m_defaultHeapBuffs[resource.internalIndex].Reset();
	else if (g_fsr2Data->m_uploadHeapBuffs[resource.internalIndex].IsInitialized())
		g_fsr2Data->m_uploadHeapBuffs[resource.internalIndex].Reset();

	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2CreatePipeline(FfxFsr2Interface* backendInterface, FfxFsr2Pass pass,
	const FfxPipelineDescription* psoDesc, FfxPipelineState* outPipeline)
{
	Assert(pass < FfxFsr2Pass::FFX_FSR2_PASS_COUNT, "Invalid FSR2 pass");
	Assert(psoDesc->samplerCount <= FSR2_Data::MAX_SAMPLERS, "Number of static samplers exceeded maximum.");
	Assert(psoDesc->rootConstantBufferCount <= FSR2_Data::MAX_NUM_CONST_BUFFERS, "Number of constant buffers exceeded maximum");

	if (g_fsr2Data->m_passes[pass].PSO && g_fsr2Data->m_passes[pass].RootSig)
	{
		outPipeline->pipeline = g_fsr2Data->m_passes[pass].PSO;
		return FFX_OK;
	}

	uint32_t flags = 0;
	flags |= FSR2_SHADER_PERMUTATION_HDR_COLOR_INPUT;
	flags |= FSR2_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS;
	flags |= FSR2_SHADER_PERMUTATION_DEPTH_INVERTED;
	flags |= FSR2_SHADER_PERMUTATION_USE_LANCZOS_TYPE;
	flags |= FSR2_SHADER_PERMUTATION_ALLOW_FP16;

	// load shader blob
	Fsr2ShaderBlobDX12 shaderBlob = g_fsr2Data->m_dll.FpGetShaderPermutation(pass, flags);
	Assert(shaderBlob.data && shaderBlob.size > 0, "Retrieving FSR2 shader failed.");

	// static samplers
	if (!g_fsr2Data->m_passes[pass].RootSig.Get())
	{
		D3D12_STATIC_SAMPLER_DESC samplers[FSR2_Data::MAX_SAMPLERS];

		const D3D12_STATIC_SAMPLER_DESC pointClampSamplerDesc
		{
			D3D12_FILTER_MIN_MAG_MIP_POINT,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			0,
			16,
			D3D12_COMPARISON_FUNC_NEVER,
			D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
			0.f,
			D3D12_FLOAT32_MAX,
			2, // s2
			0,
			D3D12_SHADER_VISIBILITY_ALL,
		};

		const D3D12_STATIC_SAMPLER_DESC linearClampSamplerDesc
		{
			D3D12_FILTER_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			0,
			16,
			D3D12_COMPARISON_FUNC_NEVER,
			D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
			0.f,
			D3D12_FLOAT32_MAX,
			3, // s3
			0,
			D3D12_SHADER_VISIBILITY_ALL,
		};

		for (uint32_t currentSamplerIndex = 0; currentSamplerIndex < psoDesc->samplerCount; ++currentSamplerIndex)
		{
			samplers[currentSamplerIndex] = psoDesc->samplers[currentSamplerIndex] == FFX_FILTER_TYPE_POINT ?
				pointClampSamplerDesc :
				linearClampSamplerDesc;
			samplers[currentSamplerIndex].ShaderRegister = currentSamplerIndex;
		}

		// root signature
		// param[0] --> UAV desc. table of size FFX_FSR2_RESOURCE_IDENTIFIER_COUNT
		// param[1] --> SRV desc. table of size FFX_FSR2_RESOURCE_IDENTIFIER_COUNT
		D3D12_ROOT_PARAMETER rootParams[FSR2_Data::MAX_ROOT_PARAMS];
		D3D12_DESCRIPTOR_RANGE descRange[FSR2_Data::MAX_DESC_RANGES];

		// UAV desc. table
		descRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		descRange[0].NumDescriptors = FFX_FSR2_RESOURCE_IDENTIFIER_COUNT;
		descRange[0].BaseShaderRegister = 0;
		descRange[0].RegisterSpace = 0;
		descRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		// SRV desc. table
		descRange[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		descRange[1].NumDescriptors = FFX_FSR2_RESOURCE_IDENTIFIER_COUNT;
		descRange[1].BaseShaderRegister = 0;
		descRange[1].RegisterSpace = 0;
		descRange[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		// root params
		int currRootParam = 0;

		rootParams[currRootParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[currRootParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParams[currRootParam].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[currRootParam].DescriptorTable.pDescriptorRanges = &descRange[0];
		currRootParam++;

		rootParams[currRootParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[currRootParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParams[currRootParam].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[currRootParam].DescriptorTable.pDescriptorRanges = &descRange[1];
		currRootParam++;

		for (uint32_t currRootConstantIdx = 0; currRootConstantIdx < psoDesc->rootConstantBufferCount; ++currRootConstantIdx)
		{
			rootParams[currRootParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			rootParams[currRootParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			rootParams[currRootParam].Constants.Num32BitValues = psoDesc->rootConstantBufferSizes[currRootConstantIdx];
			rootParams[currRootParam].Constants.ShaderRegister = currRootConstantIdx;
			rootParams[currRootParam].Constants.RegisterSpace = 0;

			currRootParam++;
		}

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc;
		rootSigDesc.NumParameters = currRootParam;
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = (UINT)psoDesc->samplerCount;
		rootSigDesc.pStaticSamplers = samplers;
		rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> outBlob, errorBlob;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, outBlob.GetAddressOf(), errorBlob.GetAddressOf());
		Check(SUCCEEDED(hr), "D3D12SerializeVersionedRootSignature() failed: %s", (char*)errorBlob->GetBufferPointer());

		auto* device = App::GetRenderer().GetDevice();
		CheckHR(device->CreateRootSignature(0, outBlob->GetBufferPointer(), outBlob->GetBufferSize(),
			IID_PPV_ARGS(g_fsr2Data->m_passes[pass].RootSig.GetAddressOf())));
	}

	// output
	outPipeline->rootSignature = g_fsr2Data->m_passes[pass].RootSig.Get();
	outPipeline->uavCount = shaderBlob.uavCount;
	outPipeline->srvCount = shaderBlob.srvCount;
	outPipeline->constCount = shaderBlob.cbvCount;

	int maxSrvSlot = -1;

	for (uint32_t srvIndex = 0; srvIndex < outPipeline->srvCount; ++srvIndex)
	{
		outPipeline->srvResourceBindings[srvIndex].slotIndex = shaderBlob.boundSRVResources[srvIndex];

		wchar_t buff[128];
		Common::CharToWideStr(shaderBlob.boundSRVResourceNames[srvIndex], buff);
		wcscpy_s(outPipeline->srvResourceBindings[srvIndex].name, buff);

		maxSrvSlot = Math::Max(maxSrvSlot, (int)shaderBlob.boundSRVResources[srvIndex]);
	}

	if(maxSrvSlot >= 0)
		g_fsr2Data->m_passes[pass].SrvTableGpuNumDescs = maxSrvSlot + 1;
		//g_fsr2Data->m_passes[pass].SrvTableGpu = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(maxSrvSlot + 1);

	int maxUavSlot = -1;

	for (uint32_t uavIndex = 0; uavIndex < outPipeline->uavCount; ++uavIndex)
	{
		outPipeline->uavResourceBindings[uavIndex].slotIndex = shaderBlob.boundUAVResources[uavIndex];

		wchar_t buff[128];
		Common::CharToWideStr(shaderBlob.boundUAVResourceNames[uavIndex], buff);
		wcscpy_s(outPipeline->uavResourceBindings[uavIndex].name, buff);

		maxUavSlot = Math::Max(maxUavSlot, (int)shaderBlob.boundUAVResources[uavIndex]);
	}

	if (maxUavSlot >= 0)
		g_fsr2Data->m_passes[pass].UavTableGpuNumDescs = maxUavSlot + 1;
		//g_fsr2Data->m_passes[pass].UavTableGpu = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(maxUavSlot + 1);

	for (uint32_t cbIndex = 0; cbIndex < outPipeline->constCount; ++cbIndex)
	{
		outPipeline->cbResourceBindings[cbIndex].slotIndex = shaderBlob.boundCBVResources[cbIndex];

		wchar_t buff[128];
		Common::CharToWideStr(shaderBlob.boundCBVResourceNames[cbIndex], buff);
		wcscpy_s(outPipeline->cbResourceBindings[cbIndex].name, buff);
	}

	// check if PSO already exists in PSO lib
	g_fsr2Data->m_passes[pass].PSO = g_fsr2Data->m_psoLib.GetComputePSO(pass,
		g_fsr2Data->m_passes[pass].RootSig.Get(), 
		Span(shaderBlob.data, shaderBlob.size));

	// to figure out each PSO corresponds to which pass
	Assert(g_fsr2Data->m_currMapIdx < FFX_FSR2_PASS_COUNT, "Invalid pass idx");
	g_fsr2Data->m_psoToPassMap[g_fsr2Data->m_currMapIdx++] = PsoMap{ .PSO = g_fsr2Data->m_passes[pass].PSO, .Pass = pass };

	outPipeline->pipeline = g_fsr2Data->m_passes[pass].PSO;

	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2DestroyPipeline(FfxFsr2Interface* backendInterface, FfxPipelineState* pipeline)
{
	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2ScheduleGpuJob(FfxFsr2Interface* backendInterface,
	const FfxGpuJobDescription* job)
{
	switch (job->jobType)
	{
	case FFX_GPU_JOB_CLEAR_FLOAT:
		RecordClearJob(job->clearJobDescriptor);
		break;
	case FFX_GPU_JOB_COMPUTE:
		RecordComputeJob(job->computeJobDescriptor);
		break;
	default:
		Assert(false, "Copy job should not reach here.");
		break;
	}

	return FFX_OK;
}

FfxErrorCode FSR2_Internal::Fsr2ExecuteGpuJobs(FfxFsr2Interface* backendInterface, FfxCommandList commandList)
{
	return FFX_OK;
}
