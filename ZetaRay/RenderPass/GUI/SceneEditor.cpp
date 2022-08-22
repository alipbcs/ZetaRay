#include "SceneEditor.h"
#include "../Win32Application/App.h"
#include "../Core/D3D12Renderer.h"
#include "../PostProcess/PostProcessPipeline.h"
#include "../Scene/Scene.h"
#include "../Scene/BVH.h"
#include "../TaskGraph/GpuTask.h"
#include "Gui.h"
#include "../Scene/RenderGraph.h"
#include "../Effects/Effects.h"
#include "../Utility/MathUtil.h"
#include <GeometricPrimitive.h>

using namespace lambda;
using namespace DirectX;


namespace
{
	class HighlightObject : public RenderNodeBase
	{
	public:
		HighlightObject(
			DirectX::EffectPipelineStateDescription& psoDesc)
			: RenderNodeBase("highlight", std::make_shared<BasicEffect>(psoDesc))
		{
			std::vector<VertexPositionNormalTexture> vertices;
			std::vector<uint16_t> indices;

			DirectX::GeometricPrimitive::CreateCube(vertices, indices);

			auto verts = MathUtil::TransformToBasicMesh(vertices);

			std::static_pointer_cast<BasicEffect>(m_effect)->SetMesh(verts, indices, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}

		~HighlightObject()
		{}

		void SetToWorld(DirectX::XMMATRIX& W)
		{
			DirectX::XMStoreFloat4x4(&m_toWorld, W);

			BasicEffect* effect = std::static_pointer_cast<BasicEffect>(m_effect).get();
		}

		void Update(double dt) override
		{
			BasicEffect* effect = std::static_pointer_cast<BasicEffect>(m_effect).get();

			effect->AddInstance(m_toWorld);
		}

		void Draw(std::shared_ptr<TGpuCommandRecorder> t) override
		{
			BasicEffect* effect = std::static_pointer_cast<BasicEffect>(m_effect).get();

			effect->DoPass(std::static_pointer_cast<TGpuCommandRecorder>(t));
		}

	private:
		const ModelNode* m_model;
		XMFLOAT4X4 m_toWorld;
	};
}


//--------------------------------------------------------------------------------------
// SceneEditor
//--------------------------------------------------------------------------------------

SceneEditor::~SceneEditor()
{
}

void SceneEditor::Init()
{
	// initialize GUI
	m_guiHandler = std::make_unique<Gui>();
	m_guiHandler->Init(g_pApp->GetHWND());
}

void SceneEditor::Clear()
{
	m_highlightRenderNode = nullptr;
	m_guiHandler->Destroy();
}

void SceneEditor::Update(double dt)
{
//	m_guiHandler->Update();
}

void SceneEditor::Render(CommandContext* context, uint32_t frameIdx)
{
	m_guiHandler->Update();
	m_guiHandler->Render(context, frameIdx);
}

XMFLOAT3 SceneEditor::GetCameraPos()
{
	return g_pApp->GetScene().GetCamera().GetPos();
}

void SceneEditor::SetCameraPos(XMFLOAT3 pos)
{
	g_pApp->GetScene().GetCamera().SetPos(XMLoadFloat3(&pos));
}

void SceneEditor::SelectObject(int posX, int posY)
{
	Scene& scene = g_pApp->GetScene();

	float P00 = scene.GetCamera().GetProj()(0, 0);
	float P11 = scene.GetCamera().GetProj()(1, 1);

	/*
		this point correspnds to a point on projection window (zNDC = 0 && zView = zNear), so to get
		the coordinates in view space, so we need the inverse projection transform and subsequent
		division by z. instead along the same ray pick a point with zView = 1 to simplify the calculations
	*/
	float xNDC = (2.0f * static_cast<float>(posX) / g_pApp->Renderer().GetBackBufferWidth()) - 1.0f;
	float yNDC = (-2.0f * static_cast<float>(posY) / g_pApp->Renderer().GetBackBufferHeight()) + 1.0f;

	XMVECTOR rayPos = XMLoadFloat3(&scene.m_camera.GetPos());
	XMVECTOR rayDir = XMVectorSet(xNDC / P00, yNDC / P11, 1.0f, 0.0f);

	// transform the ray to world space
	rayDir = XMVector3Normalize(XMVector3TransformNormal(rayDir, XMMatrixInverse(nullptr, scene.m_camera.GetViewM())));

	BoundingBox bv;
	const char* id = scene.m_bvh->CastRay(rayPos, rayDir, &bv);

	if (id)
	{
		if (!m_highlightRenderNode)
		{
			DirectX::EffectPipelineStateDescription psoDesc(
				&DirectX::VertexPositionColor::InputLayout,
				CD3DX12_BLEND_DESC(D3D12_DEFAULT),
				CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT),
				CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT));

			psoDesc.numRenderTargets = 1;
			psoDesc.rtvFormats[0] = g_pApp->Renderer().GetHdrRTFormat();
			psoDesc.dsvFormat = g_pApp->Renderer().GetDepthBufferFormat();
			psoDesc.depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			psoDesc.depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			psoDesc.blendDesc.RenderTarget[0].BlendEnable = true;
			psoDesc.blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
			psoDesc.blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
			psoDesc.blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
			psoDesc.blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
			psoDesc.blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
			psoDesc.blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
			psoDesc.blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			psoDesc.rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
			psoDesc.rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
			psoDesc.primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

			m_highlightRenderNode = std::make_shared<HighlightObject>(psoDesc);
		}

//		ModelNode* m = g_pApp->GetScene().GetModel(id);

//		XMMATRIX W = m->ToWorld();
//		ModelNode* parent = m->Parent();

//		while (parent)
//		{
			//			W = parent->ToWorld() * W;
//			parent = parent->Parent();
//		}

		XMMATRIX W =
			XMMatrixScaling(bv.Extents.x, bv.Extents.y, bv.Extents.z) *
			XMMatrixTranslation(bv.Center.x, bv.Center.y, bv.Center.z);

		std::static_pointer_cast<HighlightObject>(m_highlightRenderNode)->SetToWorld(W);

		if (m_renderNodeRemoved)
		{
			g_pApp->GetScene().AddDebugRenderNode(m_highlightRenderNode);
			m_renderNodeRemoved = false;
		}
	}
	else
	{
		if (m_highlightRenderNode)
		{
			if (!m_renderNodeRemoved)
			{
				g_pApp->GetScene().RemoveDebugRenderNode(m_highlightRenderNode);
				m_renderNodeRemoved = true;
			}
		}
	}	

	m_guiHandler->m_pickedID = id ? id : nullptr;
}

std::vector<const char*> SceneEditor::GetSpotLightList()
{
//	if(g_pApp->GetScene().m_spotlightRenderNode)
//		return g_pApp->GetScene().Gets;

	return std::vector<const char*>();
}

void SceneEditor::GetPointLightProperties(const char* id, XMFLOAT3* pos, float* RadiantIntensity, XMFLOAT3* color)
{
	/*
	id = "p1";
	auto &light = g_pApp->GetScene().m_pointlightRenderNode->m_lights[id];

	if(pos)	
		*pos = light.Pos;
	if (RadiantIntensity)
		*RadiantIntensity = light.RadiantIntensity;
	if (color)
		*color = light.Color;
		 
	*/
}

void SceneEditor::ModifyPointLight(const char* id, XMFLOAT3* pos, float* RadiantIntensity, XMFLOAT3* color)
{
}

void SceneEditor::GetSpotLightProperties(const char* id, XMFLOAT3* pos, XMFLOAT3* dir, XMFLOAT3* color, float* RadiantIntensity)
{
//	id = "s1";
//	g_pApp->GetScene().m_spotlightRenderNode->Get(id, pos, dir, color, RadiantIntensity);
}

void SceneEditor::ModifySpotLight(const char* id, XMFLOAT3* pos, XMFLOAT3* dir, DirectX::XMFLOAT3* color, float* RadiantIntensity)
{
	id = "s1";
//	auto light = g_pApp->GetScene().m_spotlightRenderNode;

//	light->Modify(id, pos, dir, color, RadiantIntensity);
}

void SceneEditor::SetToneMappingMiddleGray(float g)
{
	g_pApp->Renderer().m_postprocessPipeline->SetToneMappingMiddleGray(g);
}

void SceneEditor::SetToneMappingWhiteLuminance(float w)
{
	g_pApp->Renderer().m_postprocessPipeline->SetToneMappingWhiteLuminance(w);
}

void SceneEditor::SetBloomThreshold(float t)
{
	g_pApp->Renderer().m_postprocessPipeline->SetBloomThreshold(t);
}

void SceneEditor::SetBloomScale(float s)
{
	g_pApp->Renderer().m_postprocessPipeline->SetBloomScale(s);
}

void SceneEditor::SetPostConfig(uint16_t config)
{
	g_pApp->Renderer().m_postprocessPipeline->ModifyPipeline(config);
}

void SceneEditor::DrawSceneGraph(bool d)
{
	g_pApp->GetScene().m_bvh->Drawable(d);
}

void SceneEditor::SetFrustumCullingEnabled(bool e)
{
	g_pApp->GetScene().SetFrustumCullingEnabled(e);
}

void SceneEditor::GetFrustumCullingStats(uint32_t* numFrustumCulled, uint32_t* total)
{
	g_pApp->GetScene().m_bvh->GetStats(numFrustumCulled, total);
}

LRESULT SceneEditor::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (ImGui::GetCurrentContext() == NULL)
		return 0;

	ImGuiIO& io = ImGui::GetIO();
	//	*forward = !io.WantCaptureMouse && !io.WantCaptureKeyboard;

	switch (message)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
	{
		int button = 0;
		if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) { button = 0; }
		if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) { button = 1; }
		if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) { button = 2; }
		if (message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK) { button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 3 : 4; }
		if (!ImGui::IsAnyMouseDown() && ::GetCapture() == NULL)
			::SetCapture(hWnd);
		io.MouseDown[button] = true;
		return 0;
	}

	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP:
	{
		int button = 0;
		if (message == WM_LBUTTONUP) { button = 0; }
		if (message == WM_RBUTTONUP) { button = 1; }
		if (message == WM_MBUTTONUP) { button = 2; }
		if (message == WM_XBUTTONUP) { button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? 3 : 4; }
		io.MouseDown[button] = false;
		if (!ImGui::IsAnyMouseDown() && ::GetCapture() == hWnd)
			::ReleaseCapture();
		return 0;
	}
	
	case WM_MOUSEWHEEL:
		io.MouseWheel += (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
		return 0;
	
	case WM_MOUSEHWHEEL:
		io.MouseWheelH += (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
		return 0;
	
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wParam < 256)
			io.KeysDown[wParam] = 1;
		return 0;
	
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (wParam < 256)
			io.KeysDown[wParam] = 0;
		return 0;
	
	case WM_CHAR:
		// You can also use ToAscii()+GetKeyboardState() to retrieve characters.
		io.AddInputCharacter((unsigned int)wParam);
		return 0;
	
	case WM_SETCURSOR:
		if (LOWORD(lParam) == HTCLIENT && m_guiHandler->UpdateMouseCursor())
			return 1;
		return 0;
		//	case WM_DEVICECHANGE:
		//		if ((UINT)wParam == DBT_DEVNODES_CHANGED)
		//			g_WantUpdateHasGamepad = true;
		//		return 0;
	}

	return 0;
}