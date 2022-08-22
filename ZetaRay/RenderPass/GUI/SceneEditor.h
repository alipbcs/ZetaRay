#pragma once

#include "../lambda.h"
#include "Gui.h"


namespace lambda
{
	class RenderNodeBase;
	class CommandContext;


	class SceneEditor
	{
		friend class Scene;

	public:
		~SceneEditor();

		SceneEditor(const SceneEditor&) = delete;
		SceneEditor& operator=(const SceneEditor&) = delete;

		void Init();

		void Update(double dt);
		void Render(CommandContext* context, uint32_t frameIdx);
		void Clear();

		LRESULT WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

		DirectX::XMFLOAT3 GetCameraPos();
		void SetCameraPos(DirectX::XMFLOAT3 pos);

		void SelectObject(int posX, int posY);

//		void GetPointLightList(const char**, int *n);
		std::vector<const char*> GetSpotLightList();

		void GetPointLightProperties(const char *id, DirectX::XMFLOAT3* pos = nullptr, float* RadiantIntensity = nullptr, DirectX::XMFLOAT3* color = nullptr);
		void ModifyPointLight(const char* id, DirectX::XMFLOAT3* pos = nullptr, float* RadiantIntensity = nullptr, DirectX::XMFLOAT3* color = nullptr);
		
		void ModifySpotLight(const char* id, DirectX::XMFLOAT3* pos = nullptr, DirectX::XMFLOAT3* dir = nullptr, DirectX::XMFLOAT3* color = nullptr, float* RadiantIntensity = nullptr);
		void GetSpotLightProperties(const char* id, DirectX::XMFLOAT3* pos = nullptr, DirectX::XMFLOAT3* dir = nullptr, DirectX::XMFLOAT3* color = nullptr, float* RadiantIntensity = nullptr);

		void SetToneMappingMiddleGray(float g);
		void SetToneMappingWhiteLuminance(float w);
		void SetBloomThreshold(float t);
		void SetBloomScale(float s);
		void SetPostConfig(uint16_t config);

		void DrawSceneGraph(bool d);
		void SetFrustumCullingEnabled(bool e);
		void GetFrustumCullingStats(uint32_t* numFrustumCulled, uint32_t* total);

	private:
		SceneEditor() = default;

		std::shared_ptr<RenderNodeBase> m_highlightRenderNode;
		bool m_renderNodeRemoved = true;

		std::unique_ptr<Gui> m_guiHandler;
	};
}