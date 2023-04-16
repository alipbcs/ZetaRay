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
		ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept;

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

		static constexpr int MAX_LEN = 128;

		LogMessage() noexcept = default;
		LogMessage(const char* msg, MsgType t) noexcept;

		char Msg[MAX_LEN];
		MsgType Type;
	};

	void Init(Scene::Renderer::Interface& rendererInterface, const char* name = nullptr) noexcept;
	int Run() noexcept;
	void Abort() noexcept;

	void* AllocateFromFrameAllocator(size_t size, size_t alignment = alignof(std::max_align_t)) noexcept;

	int RegisterTask() noexcept;
	void TaskFinalizedCallback(int handle, int indegree) noexcept;
	void WaitForAdjacentHeadNodes(int handle) noexcept;
	void SignalAdjacentTailNodes(Util::Span<int> taskIDs) noexcept;

	// Submits task to priority thread pool
	void Submit(Support::Task&& t) noexcept;
	void Submit(Support::TaskSet&& ts) noexcept;
	void SubmitBackground(Support::Task&& t) noexcept;
	void FlushWorkerThreadPool() noexcept;
	void FlushAllThreadPools() noexcept;

	Core::RendererCore& GetRenderer() noexcept;
	Scene::SceneCore& GetScene() noexcept;
	const Scene::Camera& GetCamera() noexcept;
	int GetNumWorkerThreads() noexcept;
	int GetNumBackgroundThreads() noexcept;
	uint32_t GetDPI() noexcept;
	float GetUpscalingFactor() noexcept;
	void SetUpscalingEnablement(bool e) noexcept;
	bool IsFullScreen() noexcept;
	const App::Timer& GetTimer() noexcept;

	Util::Span<uint32_t> GetWorkerThreadIDs() noexcept;
	Util::Span<uint32_t> GetBackgroundThreadIDs() noexcept;
	Util::Span<uint32_t> GetAllThreadIDs() noexcept;

	void AddParam(Support::ParamVariant& p) noexcept;
	void RemoveParam(const char* group, const char* subgroup, const char* name) noexcept;
	Util::RWSynchronizedVariable<Util::Span<Support::ParamVariant>> GetParams() noexcept;

	void AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept;
	void RemoveShaderReloadHandler(const char* name) noexcept;
	Util::RSynchronizedVariable<Util::Span<ShaderReloadHandler>> GetShaderReloadHandlers() noexcept;

	// these could be implemented as template functions, but then the implementation has to be in the header,
	// which means including some heavy-to-compile headers here. Considering App.h is included in most of the 
	// codebase, this would have a measurable impact on the compile time.
	void AddFrameStat(const char* group, const char* name, int i) noexcept;
	void AddFrameStat(const char* group, const char* name, uint32_t u) noexcept;
	void AddFrameStat(const char* group, const char* name, float f) noexcept;
	void AddFrameStat(const char* group, const char* name, uint64_t f) noexcept;
	void AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total) noexcept;
	Util::RWSynchronizedVariable<Util::Span<Support::Stat>> GetStats() noexcept;
	Util::Span<float> GetFrameTimeHistory() noexcept;

	const char* GetPSOCacheDir() noexcept;
	const char* GetCompileShadersDir() noexcept;
	const char* GetAssetDir() noexcept;
	const char* GetDXCPath() noexcept;
	const char* GetToolsDir() noexcept;
	const char* GetRenderPassDir() noexcept;

	void LockStdOut() noexcept;
	void UnlockStdOut() noexcept;

	void Log(const char* msg, LogMessage::MsgType t) noexcept;
	Util::RSynchronizedVariable<Util::Span<App::LogMessage>> GetFrameLogs() noexcept;

	struct FrameAllocator
	{
		ZetaInline void* AllocateAligned(size_t size, size_t alignment) noexcept
		{
			return App::AllocateFromFrameAllocator(size, alignment);
		}

		ZetaInline void FreeAligned(void* mem, size_t size, size_t alignment) noexcept {}
	};
}
