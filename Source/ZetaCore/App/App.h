#pragma once

#include "../Support/Memory.h"
#ifndef NDEBUG
#include "../Utility/Error.h"
#endif
#include "FastDelegate/FastDelegate.h"

namespace ZetaRay::Support
{
    struct TaskSet;
    struct alignas(64) Task;
    struct ParamVariant;
    struct Stat;
}

namespace ZetaRay::App
{
    struct FrameAllocator;
}

namespace ZetaRay::Util
{
    template<typename T, Support::AllocatorType Allocator>
    class Vector;

    template<typename T>
    struct RWSynchronizedView;

    template<typename T>
    struct Span;

    template<typename T>
    struct MutableSpan;

    template<typename T>
    struct SynchronizedSpan;

    template<typename T>
    struct SynchronizedMutableSpan;

    struct StrView;
}

namespace ZetaRay::App
{
    struct Timer;
}

namespace ZetaRay::Core
{
    class RendererCore;
}

namespace ZetaRay::Scene
{
    class SceneCore;
    class Camera;
}

namespace ZetaRay::Scene::Renderer
{
    struct Interface;
}

namespace ZetaRay::App
{
    static constexpr int FRAME_ALLOCATOR_MAX_ALLOCATION_SIZE = 512 * 1024;

    struct ShaderReloadHandler
    {
        ShaderReloadHandler() = default;
        ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg);

        static constexpr int MAX_LEN = 32;
        static constexpr uint64_t INVALID_ID = UINT64_MAX;

        uint64_t ID = INVALID_ID;
        char Name[MAX_LEN];
        fastdelegate::FastDelegate0<> Dlg;
    };

    struct LogMessage
    {
        enum MsgType
        {
            INFO,
            WARNING,
            COUNT
        };

        LogMessage() = default;
        LogMessage(const char* msg, MsgType t);

        char* Msg;
        MsgType Type;
    };

    struct CpuInfo
    {
        int NumPhysicalCores;
        int NumLogicalCores;
    };

    enum class THREAD_PRIORITY
    {
        NORMAL,
        BACKGROUND
    };

    CpuInfo GetProcessorInfo();
    ZETA_THREAD_ID_TYPE GetCurrentThreadID();
    ZETA_THREAD_ID_TYPE GetThreadID(void* handle);
    void SetThreadPriority(void* handle, THREAD_PRIORITY priority);
    void SetThreadDesc(void* handle, wchar_t* buffer);

    void Init(Scene::Renderer::Interface& rendererInterface, 
        const char* name = nullptr);
    void InitBasic();
    void ShutdownBasic();
    int Run();
    void Abort();

    void* AllocateFrameAllocator(size_t size, 
        size_t alignment = alignof(std::max_align_t));

    int RegisterTask();
    void TaskFinalizedCallback(int handle, int indegree);
    void WaitForAdjacentHeadNodes(int handle);
    void SignalAdjacentTailNodes(Util::Span<int> taskIDs);

    // Submits task to priority thread pool
    void Submit(Support::Task&& t);
    void Submit(Support::TaskSet&& ts);
    void SubmitBackground(Support::Task&& t);
    void FlushWorkerThreadPool();
    void FlushAllThreadPools();

    Core::RendererCore& GetRenderer();
    Scene::SceneCore& GetScene();
    const Scene::Camera& GetCamera();
    int GetNumWorkerThreads();
    int GetNumBackgroundThreads();
    uint32_t GetDPI();
    float GetDPIScaling();
    float GetUpscalingFactor();
    void SetUpscaleFactor(float f);
    bool IsFullScreen();
    const App::Timer& GetTimer();

    Util::Span<ZETA_THREAD_ID_TYPE> GetWorkerThreadIDs();
    Util::Span<ZETA_THREAD_ID_TYPE> GetBackgroundThreadIDs();
    Util::Span<ZETA_THREAD_ID_TYPE> GetAllThreadIDs();

    void AddParam(Support::ParamVariant& p);
    void TryAddParam(Support::ParamVariant& p);
    void RemoveParam(const char* group, const char* subgroup, const char* name);
    Util::SynchronizedMutableSpan<Support::ParamVariant> GetParams();

    void AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg);
    void RemoveShaderReloadHandler(const char* name);
    Util::SynchronizedMutableSpan<ShaderReloadHandler> GetShaderReloadHandlers();

    // These could be implemented as templated functions, but that would require the 
    // implementation to be in the header, which would then expose some heavy 
    // headers to the rest of the code base (App.h is included almost everywhere).
    void AddFrameStat(const char* group, const char* name, int i);
    void AddFrameStat(const char* group, const char* name, uint32_t u);
    void AddFrameStat(const char* group, const char* name, float f);
    void AddFrameStat(const char* group, const char* name, uint64_t f);
    void AddFrameStat(const char* group, const char* name, uint32_t num, 
        uint32_t total);
    Util::SynchronizedSpan<Support::Stat> GetStats();
    Util::Span<float> GetFrameTimeHistory();

    const char* GetPSOCacheDir();
    const char* GetCompileShadersDir();
    const char* GetAssetDir();
    const char* GetDXCPath();
    const char* GetToolsDir();
    const char* GetRenderPassDir();

    void LockStdOut();
    void UnlockStdOut();

    void Log(const char* msg, LogMessage::MsgType t);
    Util::RWSynchronizedView<Util::Vector<App::LogMessage, Support::SystemAllocator>> GetLogs();
    // Note: not thread safe.
    void CopyToClipboard(Util::StrView data);

    struct FrameAllocator
    {
        ZetaInline void* AllocateAligned(size_t size, size_t alignment)
        {
            return App::AllocateFrameAllocator(size, alignment);
        }

        ZetaInline void FreeAligned(void* mem, size_t size, 
            size_t alignment) {}
    };

    struct OneTimeFrameAllocatorWithFallback
    {
        ZetaInline void* AllocateAligned(size_t size, size_t alignment = alignof(std::max_align_t))
        {
#ifndef NDEBUG
            Assert(m_numAllocs++ == 0, "This allocator can't be used more than once.");
#endif
            if (size + alignment - 1 < App::FRAME_ALLOCATOR_MAX_ALLOCATION_SIZE)
                return App::AllocateFrameAllocator(size, alignment);

            m_usedFallback = true;
            return _aligned_malloc(size, alignment);
        }

        ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment = alignof(std::max_align_t))
        {
            if(m_usedFallback)
                _aligned_free(mem);
        }

    private:
#ifndef NDEBUG
        int m_numAllocs = 0;
#endif
        bool m_usedFallback = false;
    };
}
