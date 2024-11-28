#include "PipelineStateLibrary.h"
#include "RendererCore.h"
#include "../App/Log.h"
#include <App/Common.h>
#include <App/Timer.h>
#include <Support/Task.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;
using namespace ZetaRay::Support;

#define LOGGING 1

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

        constexpr int MAX_TO_READ = 1024;
        void* buffer = App::AllocateFrameAllocator(MAX_TO_READ, alignof(char));
        DWORD numToRead;
        if (ReadFile(readPipe, buffer, MAX_TO_READ, &numToRead, nullptr))
        {
            if (numToRead)
            {
                reinterpret_cast<char*>(buffer)[numToRead] = '\0';
                App::Log(reinterpret_cast<char*>(buffer), App::LogMessage::WARNING);
            }
        }

        CloseHandle(readPipe);
    }
}

//--------------------------------------------------------------------------------------
// PipelineStateLibrary
//--------------------------------------------------------------------------------------

PipelineStateLibrary::PipelineStateLibrary(MutableSpan<ID3D12PipelineState*> psoCache)
    : m_compiledPSOs(psoCache)
{}

PipelineStateLibrary::~PipelineStateLibrary()
{
    ClearAndFlushToDisk();
}

void PipelineStateLibrary::Init(const char* name)
{
    StackStr(filename, n, "%s.cache", name);
    m_psoLibPath1.Reset(App::GetPSOCacheDir());
    m_psoLibPath1.Append(filename);

    m_foundOnDisk = Filesystem::Exists(m_psoLibPath1.Get()) && 
        Filesystem::GetFileSize(m_psoLibPath1.Get()) > 0;

    // PSO cache exists on disk, reload it
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

            ResetToEmptyPsoLib();
        }
    }
    else
        ResetToEmptyPsoLib();
}

void PipelineStateLibrary::Reset()
{
    ClearAndFlushToDisk();
    m_psoWasReset = false;

    m_cachedBlob.free_memory();
    memset(m_compiledPSOs.data(), 0, m_compiledPSOs.size() * sizeof(ID3D12PipelineState*));
}

void PipelineStateLibrary::ResetToEmptyPsoLib()
{
    // Avoid resetting twice
    if (!m_psoWasReset)
    {
        auto* device = App::GetRenderer().GetDevice();
        CheckHR(device->CreatePipelineLibrary(nullptr, 0, 
            IID_PPV_ARGS(m_psoLibrary.ReleaseAndGetAddressOf())));

        m_psoWasReset = true;
    }

    m_needsRebuild.store(true, std::memory_order_relaxed);
}

void PipelineStateLibrary::ClearAndFlushToDisk()
{
    // Needed when:
    //  1. Cached library was invalid (e.g. driver mismatch)
    //  2. One of the PSOs didn't match (e.g. modification after library was written to disk)
    //  3. Shader hot-reload
    if (m_needsRebuild.load(std::memory_order_relaxed))
    {
        // Create an empty PSO and release the existing one
        ResetToEmptyPsoLib();

        if (m_foundOnDisk)
        {
            Filesystem::RemoveFile(m_psoLibPath1.Get());
            m_cachedBlob.free_memory();
            m_foundOnDisk = false;
        }

        // Store all the compiled PSOs in the new library
        for (int idx = 0; idx < (int)m_compiledPSOs.size(); idx++)
        {
            if (!m_compiledPSOs[idx])
                continue;

            wchar_t nameWide[8];
            StackStr(name, n, "%d", idx);
            Common::CharToWideStr(name, nameWide);

            CheckHR(m_psoLibrary->StorePipeline(nameWide, m_compiledPSOs[idx]));
        }

        m_needsRebuild.store(false, std::memory_order_relaxed);
    }

    if (m_psoLibrary)
    {
        if (!m_foundOnDisk)
        {
            const size_t serializedSize = m_psoLibrary->GetSerializedSize();
            Assert(serializedSize > 0, "Serialized size was invalid.");
            uint8_t* psoLib = (uint8_t*)malloc(serializedSize);

            CheckHR(m_psoLibrary->Serialize(psoLib, serializedSize));
            Filesystem::WriteToFile(m_psoLibPath1.Get(), psoLib, (uint32_t)serializedSize);

            free(psoLib);
        }

        m_psoLibrary = nullptr;
    }

    for (auto pso : m_compiledPSOs)
    {
        // Note: PSO array is zero initialized, so PSOs that were never compiled
        // are just NULL.
        if(pso)
            pso->Release();
    }
}

void PipelineStateLibrary::Reload(uint64_t idx, ID3D12RootSignature* rootSig, 
    const char* pathToHlsl, bool flushGpu)
{
    Filesystem::Path hlsl(App::GetRenderPassDir());
    hlsl.Append(pathToHlsl);
    Assert(Filesystem::Exists(hlsl.Get()), "Path doesn't exist: %s", hlsl.Get());

    char filename[MAX_PATH];
    hlsl.Stem(filename);

    StackStr(csoFilename, N, "%s_cs.cso", filename);
    Filesystem::Path csoPath(App::GetCompileShadersDir());
    csoPath.Append(csoFilename);

#if !defined(NDEBUG) && defined(HAS_DEBUG_SHADERS)
    StackStr(cmdLine, n, "%s -T cs_6_7 -Fo %s -E main -Zi -Od -all_resources_bound -nologo -enable-16bit-types -Qembed_debug -Qstrip_reflect -WX -HV 202x %s", App::GetDXCPath(), csoPath.Get(), hlsl.Get());
#else
    StackStr(cmdLine, n, "%s -T cs_6_7 -Fo %s -E main -all_resources_bound -nologo -enable-16bit-types -Qstrip_reflect -WX -HV 202x %s", App::GetDXCPath(), csoPath.Get(), hlsl.Get());
#endif

#if LOGGING == 1
    App::DeltaTimer timer;
    timer.Start();
#endif

    HANDLE readPipe;
    HANDLE writePipe;
    InitPipe(readPipe, writePipe);

    PROCESS_INFORMATION pi;
    STARTUPINFO si{};
    si.cb = sizeof(si);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.dwFlags = STARTF_USESTDHANDLES;
    CheckWin32(CreateProcessA(nullptr, cmdLine, nullptr, nullptr, true, CREATE_NO_WINDOW, 
        nullptr, nullptr, &si, &pi));

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ReleasePipe(readPipe, writePipe);

    // Recreate the PSO
    SmallVector<uint8_t> bytecode;
    Filesystem::LoadFromFile(csoPath.Get(), bytecode);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSig;
    desc.CS.BytecodeLength = bytecode.size();
    desc.CS.pShaderBytecode = bytecode.data();

    auto* device = App::GetRenderer().GetDevice();
    ID3D12PipelineState* pso = nullptr;
    CheckHR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));

#if LOGGING == 1
    timer.End();
    LOG_UI_INFO("Reloaded shader %s in %u [ms].", pathToHlsl, (uint32_t)timer.DeltaMilli());
#endif

    m_needsRebuild.store(true, std::memory_order_relaxed);

    ID3D12PipelineState* oldPSO = m_compiledPSOs[idx];
    Assert(oldPSO, "Reload was called for a shader that hasn't been loaded yet.");

    // GPU has to be finished with the old PSO before it can be released
    if (flushGpu)
    {
        App::GetRenderer().FlushAllCommandQueues();
        oldPSO->Release();
    }
    else
    {
        // Wait on a background thread for GPU
        Task t("WaitForGpu", TASK_PRIORITY::BACKGROUND, [oldPSO]()
            {
                auto& renderer = App::GetRenderer();

                ComPtr<ID3D12Fence> fence;
                CheckHR(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, 
                    IID_PPV_ARGS(fence.GetAddressOf())));

                const uint64_t fenceToWaitFor = 1;
                renderer.SignalDirectQueue(fence.Get(), fenceToWaitFor);

                HANDLE fenceEvent = CreateEventA(nullptr, false, false, nullptr);
                CheckWin32(fenceEvent);

                CheckHR(fence->SetEventOnCompletion(fenceToWaitFor, fenceEvent));
                WaitForSingleObject(fenceEvent, INFINITE);

                CloseHandle(fenceEvent);

                oldPSO->Release();
            });

        App::SubmitBackground(ZetaMove(t));
    }

    // Replace the old PSO
    m_compiledPSOs[idx] = pso;
}

ID3D12PipelineState* PipelineStateLibrary::CompileGraphicsPSO(uint32_t idx,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc, ID3D12RootSignature* rootSig,
    const char* pathToCompiledVS,
    const char* pathToCompiledPS)
{
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

    wchar_t nameWide[8];
    StackStr(name, n, "%u", idx);
    Common::CharToWideStr(name, nameWide);

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = m_psoWasReset ? E_INVALIDARG :
        m_psoLibrary->LoadGraphicsPipeline(nameWide, &psoDesc, IID_PPV_ARGS(&pso));

    if (hr == E_INVALIDARG)
    {
        m_needsRebuild.store(true, std::memory_order_relaxed);

        auto* device = App::GetRenderer().GetDevice();
        CheckHR(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
    }

    Assert(m_compiledPSOs[idx] == nullptr, "It's assumed that every PSO is loaded at most one time.");
    m_compiledPSOs[idx] = pso;

    return pso;
}

ID3D12PipelineState* PipelineStateLibrary::CompileComputePSO(uint32_t idx, 
    ID3D12RootSignature* rootSig, const char* pathToCompiledCS)
{
    Filesystem::Path pCs(App::GetCompileShadersDir());
    pCs.Append(pathToCompiledCS);

    SmallVector<uint8_t> bytecode;
    Filesystem::LoadFromFile(pCs.Get(), bytecode);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSig;
    desc.CS.BytecodeLength = bytecode.size();
    desc.CS.pShaderBytecode = bytecode.data();

    wchar_t nameWide[8];
    StackStr(name, n, "%u", idx);
    Common::CharToWideStr(name, nameWide);

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = m_psoWasReset ? E_INVALIDARG :
        m_psoLibrary->LoadComputePipeline(nameWide, &desc, IID_PPV_ARGS(&pso));

    if (hr == E_INVALIDARG)
    {
        m_needsRebuild.store(true, std::memory_order_relaxed);

#if LOGGING == 1
        App::DeltaTimer timer;
        timer.Start();
#endif

        auto* device = App::GetRenderer().GetDevice();
        CheckHR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));

#if LOGGING == 1
        timer.End();
        LOG_UI_INFO("Compiled shader %s in %u [ms].", pathToCompiledCS, (uint32_t)timer.DeltaMilli());
#endif
    }

    Assert(m_compiledPSOs[idx] == nullptr, "It's assumed that every PSO is loaded at most one time.");
    m_compiledPSOs[idx] = pso;

    return pso;
}

ID3D12PipelineState* PipelineStateLibrary::CompileComputePSO_MT(uint32_t idx, 
    ID3D12RootSignature* rootSig, const char* pathToCompiledCS)
{
    Filesystem::Path pCs(App::GetCompileShadersDir());
    pCs.Append(pathToCompiledCS);

    SmallVector<uint8_t> bytecode;
    Filesystem::LoadFromFile(pCs.Get(), bytecode);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSig;
    desc.CS.BytecodeLength = bytecode.size();
    desc.CS.pShaderBytecode = bytecode.data();

    wchar_t nameWide[8];
    StackStr(name, n, "%u", idx);
    Common::CharToWideStr(name, nameWide);

    // MS docs: "The pipeline library is thread-safe to use, and will internally synchronize 
    // as necessary, with one exception: multiple threads loading the same PSO (via LoadComputePipeline, 
    // LoadGraphicsPipeline, or LoadPipeline) should synchronize themselves, as this act may modify 
    // the state of that pipeline within the library in a non-thread-safe manner."
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = m_psoWasReset ? E_INVALIDARG :
        m_psoLibrary->LoadComputePipeline(nameWide, &desc, IID_PPV_ARGS(&pso));

    // A PSO with the specified name doesn’t exist, or the input desc doesn’t match the data in
    // the library. Compile the PSO and then store it in the library for next time.
    if (hr == E_INVALIDARG)
    {
        // If some PSO is modified or not found, then the PSO library needs to be recreated.
        m_needsRebuild.store(true, std::memory_order_relaxed);

        auto* device = App::GetRenderer().GetDevice();
#if LOGGING == 1
        App::DeltaTimer timer;
        timer.Start();
#endif
        CheckHR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));

#if LOGGING == 1
        timer.End();
        LOG_UI_INFO("Compiled shader %s in %u [ms].", pathToCompiledCS, (uint32_t)timer.DeltaMilli());
#endif
    }

    AcquireSRWLockExclusive(&m_mapLock);
    Assert(m_compiledPSOs[idx] == nullptr, "It's assumed that every PSO is loaded at most one time.");
    m_compiledPSOs[idx] = pso;
    ReleaseSRWLockExclusive(&m_mapLock);

    return pso;
}

ID3D12PipelineState* PipelineStateLibrary::CompileComputePSO(uint32_t idx, 
    ID3D12RootSignature* rootSig, Span<const uint8_t> compiledBlob)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSig;
    desc.CS.BytecodeLength = compiledBlob.size();
    desc.CS.pShaderBytecode = compiledBlob.data();

    wchar_t nameWide[8];
    StackStr(name, n, "%u", idx);
    Common::CharToWideStr(name, nameWide);

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = m_psoWasReset ? E_INVALIDARG : 
        m_psoLibrary->LoadComputePipeline(nameWide, &desc, IID_PPV_ARGS(&pso));

    if (hr == E_INVALIDARG)
    {
        m_needsRebuild.store(true, std::memory_order_relaxed);

        auto* device = App::GetRenderer().GetDevice();
        CheckHR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    }

    Assert(m_compiledPSOs[idx] == nullptr, "It's assumed that every PSO is loaded at most one time.");
    m_compiledPSOs[idx] = pso;

    return pso;
}
