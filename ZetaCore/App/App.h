#pragma once

#include "../Support/Memory.h"
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
	template<typename T, typename Allocator>
	class Vector;

	template<typename T>
	struct RSynchronizedView;

	template<typename T>
	struct RSynchronizedVariable;

	template<typename T>
	struct RWSynchronizedView;

	template<typename T>
	struct RWSynchronizedVariable;

	template<typename T>
	struct Span;
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
	struct ShaderReloadHandler
	{
		ShaderReloadHandler() = default;
		ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg);

		static constexpr int MAX_LEN = 32;

		uint64_t ID = uint64_t(-1);
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

		static constexpr int MAX_LEN = 512;

		LogMessage() = default;
		LogMessage(const char* msg, MsgType t);

		char Msg[MAX_LEN];
		MsgType Type;
	};

	void Init(Scene::Renderer::Interface& rendererInterface, const char* name = nullptr);
	int Run();
	void Abort();

	void* AllocateSmallFrameAllocator(size_t size, size_t alignment = alignof(std::max_align_t));
	void* AllocateLargeFrameAllocator(size_t size, size_t alignment = alignof(std::max_align_t));

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
	float GetUpscalingFactor();
	void SetUpscaleFactor(float f);
	bool IsFullScreen();
	const App::Timer& GetTimer();

	Util::Span<uint32_t> GetWorkerThreadIDs();
	Util::Span<uint32_t> GetBackgroundThreadIDs();
	Util::Span<uint32_t> GetAllThreadIDs();

	void AddParam(Support::ParamVariant& p);
	void RemoveParam(const char* group, const char* subgroup, const char* name);
	Util::RWSynchronizedVariable<Util::Span<Support::ParamVariant>> GetParams();

	void AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg);
	void RemoveShaderReloadHandler(const char* name);
	Util::RSynchronizedVariable<Util::Span<ShaderReloadHandler>> GetShaderReloadHandlers();

	// these could be implemented as template functions, but then the implementation has to be in the header,
	// which means including some heavy-to-compile headers here. Considering App.h is included in most of the 
	// codebase, this would have a measurable impact on the compile time.
	void AddFrameStat(const char* group, const char* name, int i);
	void AddFrameStat(const char* group, const char* name, uint32_t u);
	void AddFrameStat(const char* group, const char* name, float f);
	void AddFrameStat(const char* group, const char* name, uint64_t f);
	void AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total);
	Util::RWSynchronizedVariable<Util::Span<Support::Stat>> GetStats();
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
	Util::RSynchronizedVariable<Util::Span<App::LogMessage>> GetFrameLogs();

	struct FrameAllocator
	{
		ZetaInline void* AllocateAligned(size_t size, size_t alignment)
		{
			return App::AllocateSmallFrameAllocator(size, alignment);
		}

		ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment) {}
	};

	struct LargeFrameAllocator
	{
		ZetaInline void* AllocateAligned(size_t size, size_t alignment)
		{
			return App::AllocateLargeFrameAllocator(size, alignment);
		}

		ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment) {}
	};
}
