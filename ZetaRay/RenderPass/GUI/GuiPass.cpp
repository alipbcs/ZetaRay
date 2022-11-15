#include "GuiPass.h"
#include "../../Math/Vector.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../Core/Renderer.h"
#include "../../Core/CommandList.h"
#include "../../Support/Param.h"
#include "../../Support/Stat.h"
#include "../../Scene/SceneCore.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"
#include "../../Utility/SynchronizedView.h"
#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <algorithm>

using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::Math;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;

namespace
{
	void AddParamRange(Vector<ParamVariant>& params, size_t offset, size_t count) noexcept
	{
		for (size_t p = offset; p < offset + count; p++)
		{
			ParamVariant& param = params[p];

			if (param.GetType() == PARAM_TYPE::PT_enum)
			{
				auto& fp = param.GetEnum();
				int idx = fp.m_curr;
				ImGui::Combo(param.GetName(), &idx, fp.m_vals, fp.m_num);
				param.SetEnum(idx);
			}
			else if (param.GetType() == PARAM_TYPE::PT_float)
			{
				auto& fp = param.GetFloat();
				float v = fp.m_val;

				auto flags = (int)ImGuiSliderFlags_NoInput;
				if ((fp.m_max - fp.m_min) / fp.m_min >= 1000.0f)
					flags |= ImGuiSliderFlags_Logarithmic;

				if(ImGui::SliderFloat(param.GetName(), &v, fp.m_min, fp.m_max, "%.5f", flags))
					param.SetFloat(v);
			}
			else if (param.GetType() == PARAM_TYPE::PT_int)
			{
				auto& ip = param.GetInt();
				int v = ip.m_val;

				if (ImGui::SliderInt(param.GetName(), &v, ip.m_min, ip.m_max))
					param.SetInt(v);
			}
			else if (param.GetType() == PARAM_TYPE::PT_float3)
			{
				auto& fp = param.GetFloat3();
				float3 v = fp.m_val;

				if (ImGui::SliderFloat3(param.GetName(), reinterpret_cast<float*>(&v), fp.m_min, fp.m_max, "%.4f"))
					param.SetFloat3(v);
			}
			else if (param.GetType() == PARAM_TYPE::PT_unit_dir)
			{
				auto& fp = param.GetUnitDir();
				float pitch = fp.m_pitch;
				float yaw = fp.m_yaw;

				ImGui::Text("%s", param.GetName());
				bool changed = false;

				if (ImGui::SliderFloat("pitch", reinterpret_cast<float*>(&pitch), 0, PI, "%.4f"))
					changed = true;
				if (ImGui::SliderFloat("yaw", reinterpret_cast<float*>(&yaw), 0.0f, TWO_PI, "%.4f"))
					changed = true;

				if(changed)
					param.SetUnitDir(pitch, yaw);
			}
			else if (param.GetType() == PARAM_TYPE::PT_color)
			{
				auto& fp = param.GetColor();
				float3 v = fp.m_val;

				if (ImGui::ColorEdit3(param.GetName(), reinterpret_cast<float*>(&v)))
					param.SetColor(v);
			}
			else if (param.GetType() == PARAM_TYPE::PT_bool)
			{
				bool v = param.GetBool();

				if (ImGui::Checkbox(param.GetName(), &v))
					param.SetBool(v);
			}
		}
	}
}

//--------------------------------------------------------------------------------------
// GuiPass
//--------------------------------------------------------------------------------------

GuiPass::GuiPass() noexcept
	: m_rootSig(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
}

GuiPass::~GuiPass() noexcept
{
	Reset();
}

void GuiPass::Init() noexcept
{
	ImGuiIO& io = ImGui::GetIO();

	// Build texture atlas
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	auto& gpuMem = App::GetRenderer().GetGpuMemory();
	auto* device = App::GetRenderer().GetDevice();

	// Upload texture to graphics system
	{
		m_imguiFontTex = gpuMem.GetTexture2DAndInit("ImgGuiFontTex", width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, pixels);

		m_fontTexSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);

		// Create texture view
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		device->CreateShaderResourceView(m_imguiFontTex.GetResource(), &srvDesc, m_fontTexSRV.CPUHandle(0));
	}

	// root signature
	{
		// root-constants
		m_rootSig.InitAsConstants(0,				// root idx
			sizeof(cbGuiPass) / sizeof(DWORD),		// num DWORDs
			0,										// register
			0,										// register-space
			D3D12_SHADER_VISIBILITY_ALL);										

		D3D12_ROOT_SIGNATURE_FLAGS flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		auto* samplers = App::GetRenderer().GetStaticSamplers();
		s_rpObjs.Init("GuiPass", m_rootSig, RendererConstants::NUM_STATIC_SAMPLERS, samplers, flags);
	}

	// PSO
	{
		// Create the input layout
		D3D12_INPUT_ELEMENT_DESC local_layout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0,D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		// RTV & DSV formats
		D3D12_INPUT_LAYOUT_DESC inputLayout = { local_layout, 3 };
		DXGI_FORMAT rtv[1] = { RendererConstants::BACK_BUFFER_FORMAT };

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(&inputLayout,
			1,
			rtv,
			RendererConstants::DEPTH_BUFFER_FORMAT);

		// blending
		psoDesc.BlendState.RenderTarget[0].BlendEnable = true;
		psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		// rasterizer 
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		// depth/stencil
		psoDesc.DepthStencilState.DepthEnable = false;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		// use an arbitrary number as "nameID" since there's only one shader
		m_pso = s_rpObjs.m_psoLib.GetGraphicsPSO(0, psoDesc, s_rpObjs.m_rootSig.Get(), COMPILED_VS[0], COMPILED_PS[0]);
	}
}

void GuiPass::Reset() noexcept
{
	if (IsInitialized())
		s_rpObjs.Clear();

	m_imguiFontTex.Reset();
	m_fontTexSRV.Reset();

	for (int i = 0; i < RendererConstants::NUM_BACK_BUFFERS; i++)
	{
		m_imguiFrameBuffs[i].IndexBuffer.Reset();
		m_imguiFrameBuffs[i].VertexBuffer.Reset();
	}

	m_cachedTimings.free();
}

void GuiPass::UpdateBuffers() noexcept
{
	ImDrawData* draw_data = ImGui::GetDrawData();
	const int currOutIdx = App::GetRenderer().CurrOutIdx();
	auto& gpuMem = App::GetRenderer().GetGpuMemory();

	// Avoid rendering when minimized
	if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
		return;

	auto& fr = m_imguiFrameBuffs[currOutIdx];

	// Create and grow vertex/index buffers if needed
	if (fr.VertexBuffer.GetSize() == 0 || fr.NumVertices < draw_data->TotalVtxCount)
	{
		fr.NumVertices = draw_data->TotalVtxCount + 5000;
		fr.VertexBuffer = gpuMem.GetUploadHeapBuffer(fr.NumVertices * sizeof(ImDrawVert));
	}

	// Upload vertex data into a single contiguous GPU buffer
	size_t offset = 0;

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		fr.VertexBuffer.Copy(offset, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert), cmd_list->VtxBuffer.Data);
		offset += cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
	}

	if (fr.IndexBuffer.GetSize() == 0 || fr.NumIndices < draw_data->TotalIdxCount)
	{
		//SafeRelease(fr->IndexBuffer);
		fr.NumIndices = draw_data->TotalIdxCount + 10000;
		fr.IndexBuffer = gpuMem.GetUploadHeapBuffer(fr.NumIndices * sizeof(ImDrawIdx));
	}

	// Upload index data into a single contiguous GPU buffer
	offset = 0;

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		fr.IndexBuffer.Copy(offset, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx), cmd_list->IdxBuffer.Data);
		offset += cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx);
	}
}

void GuiPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	directCmdList.PIXBeginEvent("ImGui");

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	RenderSettingsWindow();
	RenderProfilerWindow();
	RenderRenderGraphWindow();
	ImGui::Render();
	UpdateBuffers();

	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
	const int currBackBuffIdx = App::GetRenderer().CurrOutIdx();

	// Rendering
	const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w,
		clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };

	ImDrawData* draw_data = ImGui::GetDrawData();
	auto& fr = m_imguiFrameBuffs[currBackBuffIdx];

	// Setup desired DX state
	// Setup orthographic projection matrix into our constant buffer
	// Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right).
	cbGuiPass cb;

	{
		float L = draw_data->DisplayPos.x;
		float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
		float T = draw_data->DisplayPos.y;
		float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
		float mvp[4][4] =
		{
			{ 2.0f / (R - L),   0.0f,           0.0f,       0.0f },
			{ 0.0f,         2.0f / (T - B),     0.0f,       0.0f },
			{ 0.0f,         0.0f,           0.5f,       0.0f },
			{ (R + L) / (L - R),  (T + B) / (B - T),    0.5f,       1.0f },
		};
		memcpy(&cb.WVP, mvp, sizeof(mvp));
	}

	cb.FontTex = m_fontTexSRV.GPUDesciptorHeapIndex();

	D3D12_VIEWPORT viewports[1] = { App::GetRenderer().GetDisplayViewport() };
	directCmdList.RSSetViewports(1, viewports);

	m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
	m_rootSig.End(directCmdList);

	// Bind shader and vertex buffers
	unsigned int stride = sizeof(ImDrawVert);
	unsigned int offset = 0;
	D3D12_VERTEX_BUFFER_VIEW vbv{};
	vbv.BufferLocation = fr.VertexBuffer.GetGpuVA() + offset;
	vbv.SizeInBytes = fr.NumVertices * stride;
	vbv.StrideInBytes = stride;

	D3D12_INDEX_BUFFER_VIEW ibv{};
	ibv.BufferLocation = fr.IndexBuffer.GetGpuVA();
	ibv.SizeInBytes = fr.NumIndices * sizeof(ImDrawIdx);
	ibv.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

	directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	directCmdList.IASetVertexAndIndexBuffers(vbv, ibv);

	Assert(m_cpuDescriptors[SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
	Assert(m_cpuDescriptors[SHADER_IN_CPU_DESC::DEPTH_BUFFER].ptr > 0, "DSV hasn't been set.");
	directCmdList.OMSetRenderTargets(1, &m_cpuDescriptors[SHADER_IN_CPU_DESC::RTV], true, nullptr);

	// Setup blend factor
	directCmdList.OMSetBlendFactor(0.0f, 0.0f, 0.0f, 0.0f);

	// Render command lists
	// (Because we merged all buffers into a single one, we maintain our own offset into them)
	int global_vtx_offset = 0;
	int global_idx_offset = 0;
	ImVec2 clip_off = draw_data->DisplayPos;

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

			// Project scissor/clipping rectangles into framebuffer space
			ImVec2 clip_min(pcmd->ClipRect.x - clip_off.x, pcmd->ClipRect.y - clip_off.y);
			ImVec2 clip_max(pcmd->ClipRect.z - clip_off.x, pcmd->ClipRect.w - clip_off.y);
			if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
				continue;

			// Apply Scissor/clipping rectangle, Bind texture, Draw
			const D3D12_RECT r = { (LONG)clip_min.x, (LONG)clip_min.y, (LONG)clip_max.x, (LONG)clip_max.y };
			directCmdList.RSSetScissorRects(1, &r);

			directCmdList.DrawIndexedInstanced(pcmd->ElemCount, 1,
				pcmd->IdxOffset + global_idx_offset,
				pcmd->VtxOffset + global_vtx_offset,
				0);
		}

		global_idx_offset += cmd_list->IdxBuffer.Size;
		global_vtx_offset += cmd_list->VtxBuffer.Size;
	}

	// this is the last RenderPass, transition to PRESENT can be done here
	// Transition the render target to the state that allows it to be presented to the display.
	auto barrier = Direct3DHelper::TransitionBarrier(App::GetRenderer().GetCurrentBackBuffer().GetResource(), 
		D3D12_RESOURCE_STATE_RENDER_TARGET, 
		D3D12_RESOURCE_STATE_PRESENT);
	directCmdList.TransitionResource(&barrier, 1);

	directCmdList.PIXEndEvent();
}

void GuiPass::RenderSettingsWindow() noexcept
{
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::Begin("Debug Window", nullptr, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::SetWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetWindowSize(ImVec2(0.19f * App::GetRenderer().GetDisplayWidth(), (float)App::GetRenderer().GetDisplayHeight() * 0.45f), 
		ImGuiCond_FirstUseEver);

	ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
	if (ImGui::BeginTabBar("MyTabBar", tab_bar_flags))
	{
		if (ImGui::BeginTabItem("Camera"))
		{
			CameraTab();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Parameters"))
		{
			ParameterTab();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Shader Hot-Reload"))
		{
			ShaderReloadTab();
			ImGui::EndTabItem();
		}

		/*
		if (ImGui::BeginTabItem("Colors"))
		{
			ImGuiStyle& style = ImGui::GetStyle();

			//static ImGuiStyle ref_saved_style;

			// Default to using internal storage as reference
			//static bool init = true;
			//if (init)
			//	ref_saved_style = style;
			//init = false;

			static int output_dest = 0;
			static bool output_only_modified = true;

			ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::Combo("##output_type", &output_dest, "To Clipboard\0To TTY\0");
			ImGui::SameLine(); ImGui::Checkbox("Only Modified Colors", &output_only_modified);

			static ImGuiTextFilter filter;
			filter.Draw("Filter colors", ImGui::GetFontSize() * 16);

			static ImGuiColorEditFlags alpha_flags = 0;
			//if (ImGui::RadioButton("Opaque", alpha_flags == ImGuiColorEditFlags_None)) { alpha_flags = ImGuiColorEditFlags_None; } ImGui::SameLine();
			//if (ImGui::RadioButton("Alpha", alpha_flags == ImGuiColorEditFlags_AlphaPreview)) { alpha_flags = ImGuiColorEditFlags_AlphaPreview; } ImGui::SameLine();
			//if (ImGui::RadioButton("Both", alpha_flags == ImGuiColorEditFlags_AlphaPreviewHalf)) { alpha_flags = ImGuiColorEditFlags_AlphaPreviewHalf; } ImGui::SameLine();

			ImGui::BeginChild("##colors", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NavFlattened);
			ImGui::PushItemWidth(-160);
			for (int i = 0; i < ImGuiCol_COUNT; i++)
			{
				const char* name = ImGui::GetStyleColorName(i);
				if (!filter.PassFilter(name))
					continue;
				ImGui::PushID(i);
				ImGui::ColorEdit4("##color", (float*)&style.Colors[i], ImGuiColorEditFlags_AlphaBar | alpha_flags);
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				ImGui::TextUnformatted(name);
				ImGui::PopID();
			}

			ImGui::PopItemWidth();
			ImGui::EndChild();

			ImGui::EndTabItem();
		}
		*/
		
		ImGui::EndTabBar();
	}

	ImGui::End();
}

void GuiPass::RenderProfilerWindow() noexcept
{
	ImGui::SetNextWindowBgAlpha(0.1f);
	ImGui::Begin("Profiler", nullptr, ImGuiWindowFlags_HorizontalScrollbar);

	const float w = App::GetRenderer().GetDisplayWidth() * 0.17f;
	const float h = App::GetRenderer().GetDisplayHeight() * 0.7f;

	ImGui::SetWindowPos(ImVec2(App::GetRenderer().GetDisplayWidth() - w, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::SetWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);

	auto& renderer = App::GetRenderer();
	auto& timer = App::GetTimer();
	ImGui::Text("Device: %s", renderer.GetDeviceDescription());
	ImGui::Text("Render Resolution: %d x %d", renderer.GetRenderWidth(), renderer.GetRenderHeight());
	ImGui::Text("Display Resolution: %d x %d (%u dpi)", renderer.GetDisplayWidth(), renderer.GetDisplayHeight(), App::GetDPI());
	ImGui::Text("#Frame: %llu", timer.GetTotalFrameCount());

	ImGui::Text("");

	if (ImGui::CollapsingHeader("Stats"), ImGuiTreeNodeFlags_DefaultOpen)
	{
		auto& stats = App::GetStats().View();

		auto func = [](Stat& s)
		{
			switch (s.GetType())
			{
			case Stat::ST_TYPE::ST_INT:
				ImGui::Text("\t\t%s: %d", s.GetName(), s.GetInt());
				break;

			case Stat::ST_TYPE::ST_UINT:
				ImGui::Text("\t\t%s: %u", s.GetName(), s.GetUInt());
				break;

			case Stat::ST_TYPE::ST_FLOAT:
				ImGui::Text("\t\t%s: %f", s.GetName(), s.GetFloat());
				break;

			case Stat::ST_TYPE::ST_UINT64:
				ImGui::Text("\t\t%s: %llu", s.GetName(), s.GetUInt64());
				break;

			case Stat::ST_TYPE::ST_RATIO:
			{
				uint32_t num;
				uint32_t total;
				s.GetRatio(num, total);

				ImGui::Text("\t\t%s: %u/%u", s.GetName(), num, total);
			}
			break;

			default:
				break;
			}
		};

		for (auto s : stats)
		{
			func(s);
		}
	}

	ImGui::Text("");

	if (ImGui::CollapsingHeader("GPU Timings"), ImGuiTreeNodeFlags_DefaultOpen)
	{
		auto frameTimeHist = App::GetFrameTimeHistory();
		//double xs[128];
		//Assert(frameTimeHist.size() < ArraySize(xs), "xs is too small.");

		//for (int i = 0; i < frameTimeHist.size(); i++)
		//	xs[i] = i;

		float max = -1.0f;
		for (auto f : frameTimeHist)
			max = std::max(max, f);

		if (ImPlot::BeginPlot("Frame Time (ms)", ImVec2(w * 0.9f, 150.0f), ImPlotFlags_NoLegend))
		{
			//ImPlot::SetupAxes("", "", 0, ImPlotAxisFlags_AutoFit);
			ImPlot::SetupAxesLimits(0, (double)frameTimeHist.size(), 0, max + 1.0, ImGuiCond_Always);
			//ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle);
			ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(0.1f, 0.35f, 0.95f, 1.0f));

			ImGuiStyle& style = ImGui::GetStyle();
			ImVec4* colors = style.Colors;
			const auto wndCol = colors[ImGuiCol_WindowBg];

			ImPlot::PushStyleColor(ImPlotCol_FrameBg, wndCol);
			ImPlot::PlotLine("", frameTimeHist.data(), (int)frameTimeHist.size());
			ImPlot::PopStyleColor();
			ImPlot::PopStyleColor();
			ImPlot::EndPlot();
		}		

		ImGui::Text("");

		GpuTimingsTab();
	}

	ImGui::End();
}

void GuiPass::RenderRenderGraphWindow() noexcept
{
	const float x = 0.19f * App::GetRenderer().GetDisplayWidth();
	const float w = 0.64f * App::GetRenderer().GetDisplayWidth();
	const float h = (float)App::GetRenderer().GetDisplayHeight();

	ImGui::SetNextWindowBgAlpha(0.8f);
	ImGui::Begin("Render Graph (Use RMB for panning)", nullptr, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::SetWindowPos(ImVec2(x, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::SetWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);

	//if(!ImGui::IsWindowCollapsed())
	App::GetScene().DebugDrawRenderGraph();

	ImGui::End();
}

void GuiPass::CameraTab() noexcept
{
	Camera& camera = App::GetScene().GetCamera();
	float3 camPos = camera.GetPos();
	float3 viewBasisX = camera.GetBasisX();
	float3 viewBasisY = camera.GetBasisY();
	float3 viewBasisZ = camera.GetBasisZ();
	ImGui::Text("Camera Position: (%.3f, %.3f, %.3f)", camPos.x, camPos.y, camPos.z);
	ImGui::Text("View Basis X: (%.3f, %.3f, %.3f)", viewBasisX.x, viewBasisX.y, viewBasisX.z);
	ImGui::Text("View Basis Y: (%.3f, %.3f, %.3f)", viewBasisY.x, viewBasisY.y, viewBasisY.z);
	ImGui::Text("View Basis Z: (%.3f, %.3f, %.3f)", viewBasisZ.x, viewBasisZ.y, viewBasisZ.z);
	ImGui::Text("Aspect Ratio: %f", camera.GetAspectRatio());
	ImGui::Text("Near Plane Z: %.3f", camera.GetNearZ());
}

void GuiPass::ParameterTab() noexcept
{
	auto paramsView = App::GetParams();
	auto& params = paramsView.View();
	char currGroup[ParamVariant::MAX_GROUP_LEN];

	ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);

	{
		std::sort(params.begin(), params.end(), [](ParamVariant& p1, ParamVariant& p2)
			{
				return strcmp(p1.GetGroup(), p2.GetGroup()) < 0;
			});

		for (size_t i = 0; i < params.size();)
		{
			ParamVariant& p = params[i];

			const char* l = p.GetGroup();
			size_t n = strlen(l);
			Assert(n < ParamVariant::MAX_GROUP_LEN - 1, "buffer overflow");
			memcpy(currGroup, p.GetGroup(), n);
			currGroup[n] = '\0';

			const size_t beg = i;

			while (i < params.size() && strcmp(params[i].GetGroup(), currGroup) == 0)
				i++;

			std::sort(params.begin() + beg, params.begin() + i, [](ParamVariant& p1, ParamVariant& p2)
				{
					return strcmp(p1.GetSubGroup(), p2.GetSubGroup()) < 0;
				});
		}
	}

	char currSubGroup[ParamVariant::MAX_SUBGROUP_LEN];

	for (size_t currGroupIdx = 0; currGroupIdx < params.size();)
	{
		ParamVariant& p = params[currGroupIdx];

		const char* l = p.GetGroup();
		size_t n = strlen(l);
		memcpy(currGroup, p.GetGroup(), n);
		currGroup[n] = '\0';

		size_t i = currGroupIdx;
		while (i < params.size() && strcmp(params[i].GetGroup(), currGroup) == 0)
			i++;

		if (ImGui::TreeNode(p.GetGroup()))
		{
			for (size_t currSubgroupIdx = currGroupIdx; currSubgroupIdx < i;)
			{
				ParamVariant& currParam = params[currSubgroupIdx];

				const char* l = currParam.GetSubGroup();
				size_t n = strlen(l);
				memcpy(currSubGroup, currParam.GetSubGroup(), n);
				currSubGroup[n] = '\0';

				size_t j = currSubgroupIdx;
				while (j < params.size() && strcmp(params[j].GetSubGroup(), currSubGroup) == 0)
					j++;

				if (ImGui::TreeNode(currParam.GetSubGroup()))
				{
					AddParamRange(params, currSubgroupIdx, j - currSubgroupIdx);
					ImGui::TreePop();
				}

				currSubgroupIdx = j;
			}
			
			ImGui::TreePop();
		}

		currGroupIdx = i;
	}

	ImGui::PopItemWidth();
}

void GuiPass::GpuTimingsTab() noexcept
{
	if (m_cachedTimings.empty() || App::GetTimer().GetTotalFrameCount() % 5 == 0)
	{
		m_cachedTimings = App::GetRenderer().GetGpuTimer().GetFrameTimings(&m_cachedNumQueries);

		if(m_cachedNumQueries > 0)
		{
			std::sort(m_cachedTimings.begin(), m_cachedTimings.begin() + m_cachedNumQueries,
				[](const GpuTimer::Timing& t0, const GpuTimer::Timing& t1)
				{
					return strcmp(t0.Name, t1.Name) < 0;
				});
		}
	}

	if (m_cachedNumQueries == 0)
		return;

	const float TEXT_BASE_HEIGHT = ImGui::GetTextLineHeightWithSpacing();
	ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | 
		ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

	// When using ScrollX or ScrollY we need to specify a size for our table container!
	// Otherwise by default the table will fit all available space, like a BeginChild() call.
	ImVec2 outer_size = ImVec2(0.0f, TEXT_BASE_HEIGHT * 10);
	if (ImGui::BeginTable("table_scrolly", 2, flags, outer_size))
	{
		ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
		ImGui::TableSetupColumn("RenderPass", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Delta (ms)", ImGuiTableColumnFlags_None);
		ImGui::TableHeadersRow();

		ImU32 row_bg_color = ImGui::GetColorU32(ImVec4(0.1f, 0.4f, 0.1f, 1.0f));
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0 + 0, row_bg_color);

		for (int row = 0; row < m_cachedNumQueries; row++)
		{
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", m_cachedTimings[row].Name);

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.4f", (float)m_cachedTimings[row].Delta);
		}

		ImGui::EndTable();
	}
}

void GuiPass::ShaderReloadTab() noexcept
{
	auto reloadHandlers = App::GetShaderReloadHandlers();

	ImGui::Text("Select a shader to reload:");

	// TODO m_currShader becomes invalid when there's been a change in reloadHandlers 
	if (ImGui::BeginCombo("shader", m_currShader >= 0 ? reloadHandlers.View()[m_currShader].Name : "None", 0))
	{
		int i = 0;

		for (auto& handler : reloadHandlers.View())
		{
			bool selected = (m_currShader == i);
			if (ImGui::Selectable(handler.Name, selected))
			{
				m_currShader = i;
			}

			if(selected)
				ImGui::SetItemDefaultFocus();

			i++;
		}

		ImGui::EndCombo();
	}

	ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(4 / 7.0f, 0.8f, 0.8f));

	if (ImGui::Button("Reload") && m_currShader != -1)
	{
		reloadHandlers.View()[m_currShader].Dlg();
	}

	ImGui::PopStyleColor();
}
