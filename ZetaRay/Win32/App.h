#pragma once

//#include "../Core/ZetaRay.h"
#include "../SupportSystem/Memory.h"
#include <FastDelegate/FastDelegate.h>

namespace ZetaRay::Support
{
	struct TaskSet;
	struct alignas(64) Task;
	struct ParamVariant;
	struct Stat;
}

namespace ZetaRay::App
{
	struct PoolAllocator;
}

namespace ZetaRay::Util
{
	template<typename T, typename Allocator, size_t Alignment>
	class Vector;

	template<typename T>
	struct RSynchronizedView;

	template<typename T>
	struct RWSynchronizedView;

	template<typename T>
	struct Span;
}

namespace ZetaRay::Win32
{
	struct Timer;
}

namespace ZetaRay::Core
{
	class Renderer;
}

namespace ZetaRay::Scene
{
	class SceneCore;
}

namespace ZetaRay::App
{
	struct ShaderReloadHandler
	{
		ShaderReloadHandler() = default;
		ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept;

		static constexpr int MAX_LEN = 24;

		uint64_t ID = -1;
		char Name[MAX_LEN];
		fastdelegate::FastDelegate0<> Dlg;
	};

	void InitSimple() noexcept;
	void Init() noexcept;
	int Run() noexcept;
	void Abort() noexcept;

	void* AllocateFromMemoryPool(size_t size, const char* n = nullptr, uint32_t alignment = alignof(std::max_align_t)) noexcept;
	void FreeMemoryPool(void* pMem, size_t size, const char* n = nullptr, uint32_t alignment = alignof(std::max_align_t)) noexcept;

	// thread-safe
	int RegisterTask() noexcept;
	// thread-safe
	void TaskFinalizedCallback(int handle, int indegree) noexcept;
	// thread-safe
	void WaitForAdjacentHeadNodes(int handle) noexcept;
	void SignalAdjacentTailNodes(int* taskIDs, int n) noexcept;

	// Submits task to priority thread pool
	void Submit(Support::Task&& t) noexcept;
	void Submit(Support::TaskSet&& ts) noexcept;
	void SubmitBackground(Support::Task&& t) noexcept;
	void FlushMainThreadPool() noexcept;
	void FlushAllThreadPools() noexcept;

	//HWND GetHWND() noexcept;
	Core::Renderer& GetRenderer() noexcept;
	Scene::SceneCore& GetScene() noexcept;
	int GetNumThreads() noexcept;
	uint32_t GetDPI() noexcept;
	float GetUpscalingFactor() noexcept;
	void SetUpscalingEnablement(bool e) noexcept;
	bool IsFullScreen() noexcept;
	const Win32::Timer& GetTimer() noexcept;

	Util::Span<uint32_t> GetMainThreadIDs() noexcept;
	Util::Span<uint32_t> GetAllThreadIDs() noexcept;

	void AddParam(Support::ParamVariant& p) noexcept;
	void RemoveParam(const char* group, const char* subgroup, const char* name) noexcept;
	Util::RWSynchronizedView<Util::Vector<Support::ParamVariant, PoolAllocator, alignof(std::max_align_t)>> GetParams() noexcept;

	void AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept;
	void RemoveShaderReloadHandler(const char* name) noexcept;
	Util::RSynchronizedView<Util::Vector<ShaderReloadHandler, PoolAllocator, alignof(std::max_align_t)>> GetShaderReloadHandlers() noexcept;

	void AddFrameStat(const char* group, const char* name, int i) noexcept;
	void AddFrameStat(const char* group, const char* name, uint32_t u) noexcept;
	void AddFrameStat(const char* group, const char* name, float f) noexcept;
	void AddFrameStat(const char* group, const char* name, uint64_t f) noexcept;
	void AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total) noexcept;
	Util::RWSynchronizedView<Util::Vector<Support::Stat, PoolAllocator, alignof(std::max_align_t)>> GetStats() noexcept;
	Util::Span<double> GetFrameTimeHistory() noexcept;

	const char* GetPSOCacheDir() noexcept;
	const char* GetCompileShadersDir() noexcept;
	const char* GetAssetDir() noexcept;
	const char* GetDXCPath() noexcept;
	const char* GetToolsDir() noexcept;
	const char* GetRenderPassDir() noexcept;

	void LockStdOut() noexcept;
	void UnlockStdOut() noexcept;

	struct PoolAllocator
	{
		__forceinline void* AllocateAligned(size_t size, const char* name, int alignment) noexcept
		{
			return App::AllocateFromMemoryPool(size, name, alignment);
		}

		__forceinline void FreeAligned(void* mem, size_t size, const char* name, int alignment) noexcept
		{
			App::FreeMemoryPool(mem, size, name, alignment);
		}
	};

}
