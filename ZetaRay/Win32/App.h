#pragma once

#include "../Core/ZetaRay.h"
#include <FastDelegate/FastDelegate.h>

namespace ZetaRay
{
	class Renderer;
	class Scene;
	struct TaskSet;
	struct alignas(64) Task;
	struct ParamVariant;
	struct Stat;

	template<typename T, int Alignment>
	class Vector;

	template<typename T>
	struct RSynchronizedView;

	template<typename T>
	struct RWSynchronizedView;

	namespace Win32
	{
		struct Timer;
	}

	template<typename T>
	struct Span;

	struct ShaderReloadHandler
	{
		ShaderReloadHandler() = default;
		ShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept;

		static constexpr int MAX_LEN = 24;

		uint64_t ID = -1;
		char Name[MAX_LEN];
		fastdelegate::FastDelegate0<> Dlg;
	};

	namespace App
	{
		void InitSimple() noexcept;
		void Init() noexcept;
		int Run() noexcept;
		void Abort() noexcept;

		void* AllocateMemory(size_t size, const char* n = nullptr, int alignment = alignof(std::max_align_t)) noexcept;
		void FreeMemory(void* pMem, size_t size, const char* n = nullptr, int alignment = alignof(std::max_align_t)) noexcept;

		// thread-safe
		int RegisterTask() noexcept;
		// thread-safe
		void TaskFinalizedCallback(int handle, int indegree) noexcept;
		// thread-safe
		void WaitForAdjacentHeadNodes(int handle) noexcept;
		void SignalAdjacentTailNodes(int* taskIDs, int n) noexcept;

		// Submits task to priority thread pool
		void Submit(Task&& t) noexcept;
		void Submit(TaskSet&& ts) noexcept;
		void SubmitBackground(Task&& t) noexcept;
		void FlushMainThreadPool() noexcept;
		void FlushAllThreadPools() noexcept;

		//HWND GetHWND() noexcept;
		Renderer& GetRenderer() noexcept;
		Scene& GetScene() noexcept;
		int GetNumThreads() noexcept;
		uint32_t GetDPI() noexcept;
		float GetUpscalingFactor() noexcept;
		void SetUpscalingEnablement(bool e) noexcept;
		bool IsFullScreen() noexcept;
		const Win32::Timer& GetTimer() noexcept;

		Span<uint32_t> GetMainThreadIDs() noexcept;
		Span<uint32_t> GetAllThreadIDs() noexcept;

		void AddParam(ParamVariant& p) noexcept;
		void RemoveParam(const char* group, const char* subgroup, const char* name) noexcept;
		RWSynchronizedView<Vector<ParamVariant, alignof(std::max_align_t)>> GetParams() noexcept;

		void AddShaderReloadHandler(const char* name, fastdelegate::FastDelegate0<> dlg) noexcept;
		void RemoveShaderReloadHandler(const char* name) noexcept;
		RSynchronizedView<Vector<ShaderReloadHandler, alignof(std::max_align_t)>> GetShaderReloadHandlers() noexcept;

		void AddFrameStat(const char* group, const char* name, int i) noexcept;
		void AddFrameStat(const char* group, const char* name, uint32_t u) noexcept;
		void AddFrameStat(const char* group, const char* name, float f) noexcept;
		void AddFrameStat(const char* group, const char* name, uint64_t f) noexcept;
		void AddFrameStat(const char* group, const char* name, uint32_t num, uint32_t total) noexcept;
		RWSynchronizedView<Vector<Stat, alignof(std::max_align_t)>> GetStats() noexcept;
		Span<double> GetFrameTimeHistory() noexcept;

		const char* GetPSOCacheDir() noexcept;
		const char* GetCompileShadersDir() noexcept;
		const char* GetAssetDir() noexcept;
		const char* GetDXCPath() noexcept;
		const char* GetToolsDir() noexcept;
		const char* GetRenderPassDir() noexcept;

		void LockStdOut() noexcept;
		void UnlockStdOut() noexcept;
	};
}

