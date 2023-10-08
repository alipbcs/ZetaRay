#include "PipelineStateLibrary.h"
#include "RendererCore.h"
#include "../Utility/Utility.h"
#include "../App/Log.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

namespace
{
	ZetaInline void InitPipe(HANDLE& readPipe, HANDLE& writePipe)
	{
		SECURITY_ATTRIBUTES saAttr;
		saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
		saAttr.bInheritHandle = true;
		saAttr.lpSecurityDescriptor = nullptr;

		CheckWin32(CreatePipe(&readPipe, &writePipe, &saAttr, 0));
		CheckWin32(SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0));
	}

	ZetaInline void ReleasePipe(HANDLE readPipe, HANDLE writePipe)
	{
		CloseHandle(writePipe);

		char buffer[512];
		DWORD numToRead;
		if (ReadFile(readPipe, buffer, ZetaArrayLen(buffer), &numToRead, nullptr))
		{
			if (numToRead)
				App::Log(buffer, App::LogMessage::WARNING);
		}

		CloseHandle(readPipe);
	}
}

//--------------------------------------------------------------------------------------
// PipelineStateLibrary
//--------------------------------------------------------------------------------------

PipelineStateLibrary::~PipelineStateLibrary()
{
	ClearAndFlushToDisk();
}

void PipelineStateLibrary::Init(const char* name)
{
	StackStr(filename, n, "%s.cache", name);
	m_psoLibPath1.Reset(App::GetPSOCacheDir());
	m_psoLibPath1.Append(filename);

	m_foundOnDisk = Filesystem::Exists(m_psoLibPath1.Get()) && Filesystem::GetFileSize(m_psoLibPath1.Get()) > 0;

	// PSO cache already exists on disk, just reload it
	if (m_foundOnDisk)
	{
		Filesystem::LoadFromFile(m_psoLibPath1.Get(), m_cachedBlob);

		auto* device = App::GetRenderer().GetDevice();
		HRESULT hr = device->CreatePipelineLibrary(m_cachedBlob.data(), m_cachedBlob.size(),
			IID_PPV_ARGS(m_psoLibrary.GetAddressOf()));

		if (FAILED(hr))
		{
			if (hr == E_INVALIDARG)
				LOG_UI_INFO("PSO cache %s is corrupted.\n", m_psoLibPath1.Get());

			if (hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH)
				LOG_UI_INFO("PSO cache %s has driver mismatch.\n", m_psoLibPath1.Get());

			if (hr == D3D12_ERROR_ADAPTER_NOT_FOUND)
				LOG_UI_INFO("PSO cache %s was created using a different hardware than the one being used right now.\n", m_psoLibPath1.Get());

			Check(hr == E_INVALIDARG || hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH || hr == D3D12_ERROR_ADAPTER_NOT_FOUND,
				"CreatePipelineLibrary() failed with HRESULT %d", hr);

			DeletePsoLibFile();
			ResetPsoLib();
		}
	}
	else
		ResetPsoLib();
}

void PipelineStateLibrary::ClearAndFlushToDisk()
{
	if (m_psoLibrary)
	{
		if (!m_foundOnDisk)
		{
			size_t serializedSize = m_psoLibrary->GetSerializedSize();
			uint8_t* psoLib = new(std::nothrow) uint8_t[serializedSize];

			CheckHR(m_psoLibrary->Serialize(psoLib, serializedSize));
			Filesystem::WriteToFile(m_psoLibPath1.Get(), psoLib, (uint32_t)serializedSize);

			delete[] psoLib;
		}

		m_psoLibrary = nullptr;
	}

	for (auto e : m_compiledPSOs)
		e.PSO->Release();

	m_compiledPSOs.free_memory();
	m_cachedBlob.free_memory();
}

void PipelineStateLibrary::DeletePsoLibFile()
{
	if (m_foundOnDisk)
	{
		Filesystem::RemoveFile(m_psoLibPath1.Get());
		m_cachedBlob.free_memory();
		m_foundOnDisk = false;
	}

	// dont't clear the already compiled PSOs, those are still valid!
	//m_compiledPSOs.clear();
}

void PipelineStateLibrary::ResetPsoLib(bool forceReset)
{
	// avoid recreating PipelineStateLibrary object if it's been reset since the start of program
	if (forceReset || !m_psoWasReset)
	{
		auto* device = App::GetRenderer().GetDevice();
		CheckHR(device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(m_psoLibrary.ReleaseAndGetAddressOf())));

		m_psoWasReset = true;
	}
}

void PipelineStateLibrary::Reload(uint64_t nameID, const char* pathToHlsl, bool isComputePSO)
{
//	ID3D12PipelineState* pso = Find(nameID);
//	if (!pso)
//		return;

	Filesystem::Path hlsl(App::GetRenderPassDir());
	hlsl.Append(pathToHlsl);

	Assert(Filesystem::Exists(hlsl.Get()), "unable to find path %s", hlsl.Get());

	char filename[MAX_PATH];
	hlsl.Stem(filename);

	Filesystem::Path outPath(App::GetCompileShadersDir());
	outPath.Append(filename);

	if (isComputePSO)
	{
		// dxc.exe -T cs_6_6 -Fo <shader>_cs.csp -E main ...
#ifdef _DEBUG
		StackStr(cmdLine, n, "%s -T cs_6_6 -Fo %s_cs.cso -E main -Zi -Od -all_resources_bound -nologo -enable-16bit-types -Qembed_debug -Qstrip_reflect -WX -HV 2021 %s", App::GetDXCPath(), outPath.Get(), hlsl.Get());
#else
		StackStr(cmdLine, n, "%s -T cs_6_6 -Fo %s_cs.cso -E main -all_resources_bound -nologo -enable-16bit-types -Qstrip_reflect -WX -HV 2021 %s", App::GetDXCPath(), outPath.Get(), hlsl.Get());
#endif // _DEBUG

		HANDLE readPipe;
		HANDLE writePipe;
		InitPipe(readPipe, writePipe);

		PROCESS_INFORMATION pi;
		STARTUPINFO si{};
		si.cb = sizeof(si);
		si.hStdOutput = writePipe;
		si.hStdError = writePipe;
		si.dwFlags = STARTF_USESTDHANDLES;
		CheckWin32(CreateProcessA(nullptr, cmdLine, nullptr, nullptr, true, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));

		// TODO following by itself is not enogh to guarantee PSO is not deleted while GPU is still referenceing 
		// it. But since the calling thread blocks until PSO is compiled and processing for next frame won't 
		// start until previous frame is done, it happens to work
		App::GetRenderer().FlushAllCommandQueues();

		DeletePsoLibFile();
		ResetPsoLib(true);
		m_compiledPSOs.clear();

		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);

		ReleasePipe(readPipe, writePipe);
	}
	else
	{
		STARTUPINFO siVS{};
		PROCESS_INFORMATION piVS;

		STARTUPINFO siPS{};
		PROCESS_INFORMATION piPS;

		App::GetRenderer().FlushAllCommandQueues();

		DeletePsoLibFile();
		ResetPsoLib(true);
		m_compiledPSOs.clear();

		// VS
		{
#ifdef _DEBUG
			StackStr(cmdLine, n, "%s -T vs_6_6 -Fo %s_vs.cso -E mainVS -Zi -Od -all_resources_bound -nologo -enable-16bit-types -Qembed_debug -Qstrip_reflect -WX -HV 2021 %s", App::GetDXCPath(), outPath.Get(), hlsl.Get());
#else
			StackStr(cmdLine, n, "%s -T vs_6_6 -Fo %s_vs.cso -E mainVS -all_resources_bound -nologo -enable-16bit-types -Qstrip_reflect -WX -HV 2021%s", App::GetDXCPath(), outPath.Get(), hlsl.Get());
#endif // _DEBUG

			siVS.cb = sizeof(siVS);
			CheckWin32(CreateProcessA(nullptr, cmdLine, nullptr, nullptr, false, CREATE_NO_WINDOW, nullptr, nullptr, &siVS, &piVS));
		}

		HANDLE readPipe;
		HANDLE writePipe;
		InitPipe(readPipe, writePipe);
		siPS.hStdOutput = writePipe;
		siPS.hStdError = writePipe;
		siPS.dwFlags = STARTF_USESTDHANDLES;

		// PS
		{
#ifdef _DEBUG
			StackStr(cmdLine, n, "%s -T ps_6_6 -Fo %s_ps.cso -E mainPS -Zi -Od -all_resources_bound -nologo -enable-16bit-types -Qembed_debug -Qstrip_reflect -WX -HV 2021 %s", App::GetDXCPath(), outPath.Get(), hlsl.Get());
#else
			StackStr(cmdLine, n, "%s -T ps_6_6 -Fo %s_ps.cso -E mainPS -all_resources_bound -nologo -enable-16bit-types -Qstrip_reflect -WX -HV 2021 %s", App::GetDXCPath(), outPath.Get(), hlsl.Get());
#endif // _DEBUG

			siPS.cb = sizeof(siPS);
			CheckWin32(CreateProcessA(nullptr, cmdLine, nullptr, nullptr, true, CREATE_NO_WINDOW, nullptr, nullptr, &siPS, &piPS));
		}

		HANDLE pis[] = { piVS.hProcess, piPS.hProcess };
		WaitForMultipleObjects(ZetaArrayLen(pis), pis, true, INFINITE);

		CloseHandle(piVS.hThread);
		CloseHandle(piVS.hProcess);

		CloseHandle(piPS.hThread);
		CloseHandle(piPS.hProcess);

		ReleasePipe(readPipe, writePipe);
	}
}

ID3D12PipelineState* PipelineStateLibrary::Find(uint64_t key)
{
	if (m_compiledPSOs.empty())
		return nullptr;

	auto idx = BinarySearch(Span(m_compiledPSOs), key, [](Entry& e) {return e.Key; });
	if (idx != -1)
		return m_compiledPSOs[idx].PSO;

	return nullptr;
}

bool PipelineStateLibrary::UpdatePSO(Entry e)
{
	if (m_compiledPSOs.empty())
		return false;

	auto i = BinarySearch(Span(m_compiledPSOs), e.Key, [](Entry& e) {return e.Key; });

	if (i != -1)
	{
		Assert(m_compiledPSOs[i].PSO, "PSO was NULL");
		m_compiledPSOs[i].PSO->Release();
		m_compiledPSOs[i].PSO = e.PSO;

		return true;
	}

	return false;
}

bool PipelineStateLibrary::RemovePSO(uint64_t nameID)
{
	if (m_compiledPSOs.empty())
		return false;

	auto i = BinarySearch(Span(m_compiledPSOs), nameID, [](Entry& e) {return e.Key; });

	if (i == -1)
		return false;

	m_compiledPSOs.erase(i);

	return true;
}

void PipelineStateLibrary::InsertPSOAndKeepSorted(Entry e)
{
	// if PSO with given key already exists, just update it
	if (UpdatePSO(e))
		return;

	m_compiledPSOs.push_back(e);

	const int n = (int)m_compiledPSOs.size();

	// insertion sort
	int idx = n - 1;
	while (idx > 0 && m_compiledPSOs[idx - 1].Key < m_compiledPSOs[idx].Key)
	{
		std::swap(m_compiledPSOs[idx - 1], m_compiledPSOs[idx]);
		idx--;
	}
}

ID3D12PipelineState* PipelineStateLibrary::GetGraphicsPSO(uint64_t nameID,
	D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc,
	ID3D12RootSignature* rootSig, 
	const char* pathToCompiledVS,
	const char* pathToCompiledPS)
{
	// if the PSO was already created, just return it
	ID3D12PipelineState* pso = Find(nameID);
	
	if (pso)
		return pso;

	Assert(pathToCompiledVS, "path to vertex shader was NULL");
	Assert(pathToCompiledPS, "path to pixel shader was NULL");

	SmallVector<uint8_t> vsBytecode;
	SmallVector<uint8_t> psBytecode;

	{
		Filesystem::Path pVs(App::GetCompileShadersDir());
		pVs.Append(pathToCompiledVS);

		Filesystem::LoadFromFile(pVs.Get(), vsBytecode);
	}

	{
		Filesystem::Path pPs(App::GetCompileShadersDir());
		pPs.Append(pathToCompiledPS);

		Filesystem::LoadFromFile(pPs.Get(), psBytecode);
	}

	psoDesc.VS.BytecodeLength = vsBytecode.size();
	psoDesc.VS.pShaderBytecode = vsBytecode.data();
	psoDesc.PS.BytecodeLength = psBytecode.size();
	psoDesc.PS.pShaderBytecode = psBytecode.data();
	psoDesc.pRootSignature = rootSig;

	wchar_t nameWide[32];
	swprintf(nameWide, sizeof nameWide / sizeof(wchar_t), L"%llu", nameID);

	Assert(m_psoLibrary, "m_psoLibrary has not been created yet.");
	HRESULT hr = m_psoLibrary->LoadGraphicsPipeline(nameWide, &psoDesc, IID_PPV_ARGS(&pso));

	// A PSO with the specified name doesn’t exist, or the input desc doesn’t match the data in the library.
	// Create the PSO and store it in the library for possible future reuse.
	if (hr == E_INVALIDARG)
	{
		// clear old pso libray (if it exists) 
		DeletePsoLibFile();
		// set the ID3D12PipelineLibrary to empty, but only if it's non-empty
		ResetPsoLib();

		auto* device = App::GetRenderer().GetDevice();
			
		// can cause a hitch as the CPU is stalled
		CheckHR(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
		CheckHR(m_psoLibrary->StorePipeline(nameWide, pso));
	}

	Entry e;
	e.Key = nameID;
	e.PSO = pso;

	InsertPSOAndKeepSorted(e);

	return pso;
}

ID3D12PipelineState* PipelineStateLibrary::GetComputePSO(uint64_t nameID, ID3D12RootSignature* rootSig,
	const char* pathToCompiledCS)
{
	ID3D12PipelineState* pso = Find(nameID);

	if (pso)
		return pso;

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = rootSig;

	Assert(pathToCompiledCS, "path was NULL");

	Filesystem::Path pCs(App::GetCompileShadersDir());
	pCs.Append(pathToCompiledCS);
		
	SmallVector<uint8_t> bytecode;
	Filesystem::LoadFromFile(pCs.Get(), bytecode);

	desc.CS.BytecodeLength = bytecode.size();
	desc.CS.pShaderBytecode = bytecode.data();

	wchar_t nameWide[32];
	int n = swprintf(nameWide, (sizeof nameWide / sizeof(wchar_t)) - 1, L"%llu", nameID);
	Assert(n > 0, "swprintf failed.");

	HRESULT hr = m_psoLibrary->LoadComputePipeline(nameWide, &desc, IID_PPV_ARGS(&pso));

	// A PSO with the specified name doesn’t exist, or the input desc doesn’t match the data in the library.
	// Create the PSO and then store it in the library for next time.
	if (hr == E_INVALIDARG)
	{
		DeletePsoLibFile();
		ResetPsoLib();

		// can cause a hitch as the CPU is stalled
		auto* device = App::GetRenderer().GetDevice();

		CheckHR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
		CheckHR(m_psoLibrary->StorePipeline(nameWide, pso));
	}

	Entry e;
	e.Key = nameID;
	e.PSO = pso;

	InsertPSOAndKeepSorted(e);

	return pso;
}

ID3D12PipelineState* PipelineStateLibrary::GetComputePSO(uint64_t nameID, ID3D12RootSignature* rootSig, 
	Span<const uint8_t> compiledBlob)
{
	// if the PSO has already been created, just return it
	ID3D12PipelineState* pso = Find(nameID);

	if (pso)
		return pso;

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
	desc.pRootSignature = rootSig;
	desc.CS.BytecodeLength = compiledBlob.size();
	desc.CS.pShaderBytecode = compiledBlob.data();
	desc.NodeMask = 0;
	desc.CachedPSO.CachedBlobSizeInBytes = 0;
	desc.CachedPSO.pCachedBlob = nullptr;
	desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	wchar_t nameWide[32];
	swprintf(nameWide, sizeof nameWide / sizeof(wchar_t), L"%llu", nameID);

	HRESULT hr = m_psoLibrary->LoadComputePipeline(nameWide, &desc, IID_PPV_ARGS(&pso));

	// A PSO with the specified name doesn’t exist, or the input desc doesn’t match the data in the library.
	// Create the PSO and then store it in the library for next time.
	if (hr == E_INVALIDARG)
	{
		// clear old pso libray (if it exists) 
		DeletePsoLibFile();
		// set the ID3D12PipelineLibrary to empty, but only if it's non-empty
		ResetPsoLib();

		auto* device = App::GetRenderer().GetDevice();

		// can cause a hitch as the CPU is stalled
		CheckHR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
		CheckHR(m_psoLibrary->StorePipeline(nameWide, pso));
	}

	Entry e;
	e.Key = nameID;
	e.PSO = pso;

	InsertPSOAndKeepSorted(e);

	return pso;
}
