#include "GuiPass.h"
#include <Math/Vector.h>
#include <Core/Direct3DHelpers.h>
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Support/Param.h>
#include <Support/Stat.h>
#include <Scene/SceneCore.h>
#include <App/Timer.h>
#include <Utility/SynchronizedView.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <algorithm>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core::Direct3DHelper;
using namespace ZetaRay::Math;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;

namespace
{
	void AddParamRange(Span<ParamVariant> params, size_t offset, size_t count) noexcept
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
				//if ((fp.m_max - fp.m_min) / Math::Max(fp.m_min, 1e-6f) >= 1000.0f)
				if (fp.m_stepSize <= 1e-3f)
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

	void DrawAxis(const char* label, const float3& axis, const float3& color, float lineWidth) noexcept
	{
		// axis
		float axis_x[2];
		float axis_y[2];

		// starting point
		axis_x[0] = 0.0f;
		axis_y[0] = 0.0f;

		// end point
		axis_x[1] = axis.x;
		axis_y[1] = axis.z;

		ImPlot::SetNextLineStyle(ImVec4(color.x, color.y, color.z, 1.0f), lineWidth);
		ImPlot::PlotLine(label, axis_x, axis_y, ZetaArrayLen(axis_x));

		// arrow tip
		constexpr float arrowLenX = 0.05f;
		constexpr float arrowLenY = 0.1f;

		float arrow_x[3];
		float arrow_y[3];

		// starting point
		arrow_x[0] = 0.0f - arrowLenX;
		arrow_y[0] = 1.0f - arrowLenY;

		// middle point
		arrow_x[1] = axis_x[1];
		arrow_y[1] = axis_y[1];

		// end point
		arrow_x[2] = 0.0f + arrowLenX;
		arrow_y[2] = 1.0f - arrowLenY;

		// rotate
		float2 rotMatRow1 = float2(axis.z, axis.x);
		float2 rotMatRow2 = float2(-axis.x, axis.z);

		float2 rotated;
		rotated.x = rotMatRow1.x * arrow_x[0] + rotMatRow1.y * arrow_y[0];
		rotated.y = rotMatRow2.x * arrow_x[0] + rotMatRow2.y * arrow_y[0];
		arrow_x[0] = rotated.x;
		arrow_y[0] = rotated.y;

		rotated.x = rotMatRow1.x * arrow_x[2] + rotMatRow1.y * arrow_y[2];
		rotated.y = rotMatRow2.x * arrow_x[2] + rotMatRow2.y * arrow_y[2];
		arrow_x[2] = rotated.x;
		arrow_y[2] = rotated.y;

		ImPlot::SetNextLineStyle(ImVec4(color.x, color.y, color.z, 1.0f), lineWidth);
		ImPlot::PlotLine("", arrow_x, arrow_y, ZetaArrayLen(arrow_x));
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
	// [hack] RenderPasses and App should be decoupled -- in the event of adding/removing fonts,
	// ImGui expects the font texture to be rebuilt before ImGui::NewFrame() is called, whereas 
	// RenderPasses are updated later in the frame. As a workaround, store a delegate to rebuild 
	// method so that App can call it directly
	m_font.RebuildFontTexDlg = fastdelegate::MakeDelegate(this, &GuiPass::RebuildFontTex);

	ImGuiIO& io = ImGui::GetIO();
	io.UserData = reinterpret_cast<void*>(&m_font.RebuildFontTexDlg);

	RebuildFontTex();

	// root signature
	{
		// root constants
		m_rootSig.InitAsConstants(0,				// root idx
			sizeof(cbGuiPass) / sizeof(DWORD),		// num DWORDs
			0,										// register
			0,										// register space
			D3D12_SHADER_VISIBILITY_ALL);										

		D3D12_ROOT_SIGNATURE_FLAGS flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		auto samplers = App::GetRenderer().GetStaticSamplers();
		s_rpObjs.Init("GuiPass", m_rootSig, samplers.size(), samplers.data(), flags);
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
		DXGI_FORMAT rtv[1] = { Constants::BACK_BUFFER_FORMAT };

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(&inputLayout,
			1,
			rtv,
			Constants::DEPTH_BUFFER_FORMAT);

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

	for (int i = 0; i < Constants::NUM_BACK_BUFFERS; i++)
	{
		m_imguiFrameBuffs[i].IndexBuffer.Reset();
		m_imguiFrameBuffs[i].VertexBuffer.Reset();
	}

	m_cachedTimings.free_memory();
}

void GuiPass::UpdateBuffers() noexcept
{
	ImDrawData* draw_data = ImGui::GetDrawData();
	const int currOutIdx = App::GetRenderer().GetCurrentBackBufferIndex();
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

void GuiPass::Update() noexcept
{
	if (m_font.IsStale)
	{
		// Upload texture to GPU
		Assert(m_font.Pixels, "pointer to pixels was NULL.");
		Assert(m_font.Width && m_font.Height, "invalid texture dims.");

		auto& gpuMem = App::GetRenderer().GetGpuMemory();
		m_imguiFontTex = gpuMem.GetTexture2DAndInit("ImGuiFontTex", m_font.Width, m_font.Height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_font.Pixels);

		m_fontTexSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);

		// Create texture view
		Direct3DHelper::CreateTexture2DSRV(m_imguiFontTex, m_fontTexSRV.CPUHandle(0));

		m_font.IsStale = false;
	}
}

void GuiPass::Render(CommandList& cmdList) noexcept
{
	Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
	GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

	auto& renderer = App::GetRenderer(); 
	auto& gpuTimer = renderer.GetGpuTimer();

	directCmdList.PIXBeginEvent("ImGui");

	// record the timestamp prior to execution
	const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "ImGui");

	directCmdList.SetRootSignature(m_rootSig, s_rpObjs.m_rootSig.Get());
	directCmdList.SetPipelineState(m_pso);

	RenderSettings();
	RenderLogWindow();
	ImGui::Render();
	UpdateBuffers();

	const int currBackBuffIdx = renderer.GetCurrentBackBufferIndex();

	// Rendering
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

	D3D12_VIEWPORT viewports[1] = { renderer.GetDisplayViewport() };
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

	// record the timestamp after execution
	gpuTimer.EndQuery(directCmdList, queryIdx);

	// [hack] this is the last RenderPass, transition to PRESENT can be done here
	// Transition the render target to the state that allows it to be presented to the display.
	directCmdList.ResourceBarrier(renderer.GetCurrentBackBuffer().GetResource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT);

	directCmdList.PIXEndEvent();
}

void GuiPass::RenderSettings() noexcept
{
	const int displayWidth = App::GetRenderer().GetDisplayWidth();
	const int displayHeight = App::GetRenderer().GetDisplayHeight();

	ImGui::Begin("Debug Window", nullptr, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
	ImGui::SetWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
	ImGui::SetWindowSize(ImVec2(m_dbgWndWidthPct * displayWidth, m_dbgWndHeightPct * displayHeight),
		ImGuiCond_Always);

	//if (ImGui::CollapsingHeader("Info", ImGuiTreeNodeFlags_DefaultOpen))
	if (ImGui::CollapsingHeader("Info", ImGuiTreeNodeFlags_DefaultOpen))
	{
		InfoTab();
		ImGui::Text("");
	}

	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
	{
		CameraTab();
		ImGui::Text("");
	}

	if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_None))
	{
		ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
		if (ImGui::BeginTabBar("MyTabBar", tab_bar_flags))
		{
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

		ImGui::Text("");
	}

	RenderProfiler();

	ImGui::End();
}

void GuiPass::RenderProfiler() noexcept
{
	const float w = App::GetRenderer().GetDisplayWidth() * m_dbgWndWidthPct;
	//const float h = App::GetRenderer().GetDisplayHeight() * 0.7f;

	auto& renderer = App::GetRenderer();
	auto& timer = App::GetTimer();

	if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_None))
	{
		//ImGui::Text("Device: %s", renderer.GetDeviceDescription());
		//ImGui::Text("Render Resolution: %d x %d", renderer.GetRenderWidth(), renderer.GetRenderHeight());
		//ImGui::Text("Display Resolution: %d x %d (%u dpi)", renderer.GetDisplayWidth(), renderer.GetDisplayHeight(), App::GetDPI());
		ImGui::Text("Frame %llu", timer.GetTotalFrameCount());

		ImGui::SameLine();
		ImGui::Text("		");
		ImGui::SameLine();

		if (ImGui::Button("Visualize RenderGraph"))
			m_showRenderGraph = true;

		if (m_showRenderGraph)
			RenderRenderGraph();

		Span<Support::Stat> stats = App::GetStats().Variable();

		auto func = [](Stat& s)
		{
			switch (s.GetType())
			{
			case Stat::ST_TYPE::ST_INT:
				ImGui::Text("\t%s: %d", s.GetName(), s.GetInt());
				break;

			case Stat::ST_TYPE::ST_UINT:
				ImGui::Text("\t%s: %u", s.GetName(), s.GetUInt());
				break;

			case Stat::ST_TYPE::ST_FLOAT:
				ImGui::Text("\t%s: %f", s.GetName(), s.GetFloat());
				break;

			case Stat::ST_TYPE::ST_UINT64:
				ImGui::Text("\t%s: %llu", s.GetName(), s.GetUInt64());
				break;

			case Stat::ST_TYPE::ST_RATIO:
			{
				uint32_t num;
				uint32_t total;
				s.GetRatio(num, total);

				ImGui::Text("\t%s: %u/%u", s.GetName(), num, total);
			}
			break;

			default:
				break;
			}
		};

		for (auto s : stats)
			func(s);
		
		ImGui::Text(""); 
	}

	if (ImGui::CollapsingHeader("GPU Timings", ImGuiTreeNodeFlags_DefaultOpen))
	{
		auto frameTimeHist = App::GetFrameTimeHistory();

		float max_ = 0.0f;
		for (auto f : frameTimeHist)
			max_ = Math::Max(max_, f);

		if (ImPlot::BeginPlot("Frame Time (ms)", ImVec2(w * 0.9f, 150.0f), ImPlotFlags_NoLegend))
		{
			ImPlot::SetupAxes("", "", 0, ImPlotAxisFlags_NoHighlight);
			ImPlot::SetupAxesLimits(0, (double)frameTimeHist.size(), 0, max_ + 1.0, ImGuiCond_Always);
			//ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle);
			ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(0.1f, 0.35f, 0.95f, 1.0f));

			ImGuiStyle& style = ImGui::GetStyle();
			ImVec4* colors = style.Colors;
			const auto wndCol = colors[ImGuiCol_WindowBg];

			ImPlot::PushStyleColor(ImPlotCol_FrameBg, wndCol);
			ImPlot::PlotLine("", frameTimeHist.data(), (int)frameTimeHist.size());
			ImPlot::PopStyleColor();
			ImPlot::EndPlot();
		}		

		ImGui::Text("");

		GpuTimingsTab();
	}
}

void GuiPass::RenderRenderGraph() noexcept
{
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);

	if (ImGui::Begin("Render Graph (Use RMB for panning)", &m_showRenderGraph, flags))
		App::GetScene().DebugDrawRenderGraph();
	
	ImGui::End();

	////if(!ImGui::IsWindowCollapsed())
	//App::GetScene().DebugDrawRenderGraph();

	//ImGui::End();
}

void GuiPass::RenderLogWindow() noexcept
{
	auto frameLogs = App::GetFrameLogs().Variable();
	m_prevNumLogs = (int)m_logs.size();
	m_logs.append_range(frameLogs.begin(), frameLogs.end());

	const int displayWidth = App::GetRenderer().GetDisplayWidth();
	const int displayHeight = App::GetRenderer().GetDisplayHeight();

	if(!m_showLogsWindow && m_logs.size() != m_prevNumLogs)
		ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);

	if (ImGui::Begin("Logs", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar))
	{
		const float wndWidth = m_logWndWidthPct * displayWidth;
		const float wndHeight = ceilf(m_logWndHeightPct * displayHeight);

		// TODO save the last position so that ImGuiCond_Always can be removed
		ImGui::SetWindowPos(ImVec2(ceilf(m_dbgWndWidthPct * displayWidth), displayHeight - wndHeight), ImGuiCond_Always);
		ImGui::SetWindowSize(ImVec2(wndWidth, wndHeight), ImGuiCond_FirstUseEver);

		if (ImGui::Button("Clear"))
			m_logs.clear();

		ImGui::Separator();
		ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		char* buf;
		char* buf_end;

		//std::partition(m_logs.begin(), m_logs.end(),
		//	[](const App::LogMessage& m)
		//	{
		//		return m.Type == App::LogMessage::INFO;
		//	}
		//);

		// TODO consider using ImGuiListClipper
		for (auto& msg : m_logs)
		{
			ImVec4 color = msg.Type == App::LogMessage::INFO ? ImVec4(0.3f, 0.4f, 0.5f, 1.0f) : ImVec4(0.4f, 0.2f, 0.2f, 1.0f);
			ImGui::TextColored(color, msg.Msg);
		}

		ImGui::PopStyleVar();

		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(1.0f);

		ImGui::EndChild();
	}
	else
		ImGui::SetWindowPos(ImVec2(ceilf(m_dbgWndWidthPct * displayWidth), (float)displayHeight), ImGuiCond_Always);

	m_showLogsWindow = !ImGui::IsWindowCollapsed();
	ImGui::End();
}

void GuiPass::InfoTab() noexcept
{
	auto& renderer = App::GetRenderer();
	auto& timer = App::GetTimer();
	ImGui::Text("Device: %s", renderer.GetDeviceDescription());
	ImGui::Text("Render Resolution: %d x %d", renderer.GetRenderWidth(), renderer.GetRenderHeight());
	ImGui::Text("Display Resolution: %d x %d (%u dpi)", renderer.GetDisplayWidth(), renderer.GetDisplayHeight(), App::GetDPI());
	ImGui::Text("");
	ImGui::Text("Controls:");
	ImGui::Text("\t- WASD+LMB moves the camera");
	ImGui::Text("\t- MMB zooms in/out");
}

void GuiPass::CameraTab() noexcept
{
	const Camera& camera = App::GetCamera();
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

	constexpr int plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_Equal;

	if (ImPlot::BeginPlot("Coodinate System", ImVec2(250.0f, 250.0f), plotFlags))
	{
		constexpr int axisFlags = ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoHighlight;
		ImPlot::SetupAxes("X", "Z", axisFlags, axisFlags);
		ImPlot::SetupAxesLimits(-1.5f, 1.5f, -1.5f, 1.5f, ImGuiCond_Always);

		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4* colors = style.Colors;
		const auto wndCol = colors[ImGuiCol_WindowBg];
		ImPlot::PushStyleColor(ImPlotCol_FrameBg, wndCol);
		
		const float3 xAxis = camera.GetBasisX();
		const float3 zAxis = camera.GetBasisZ();
		DrawAxis("X", xAxis, float3(0.99f, 0.15f, 0.05f), 3.0f);
		DrawAxis("Z", zAxis, float3(0.1f, 0.5f, 0.99f), 3.0f);

		ImPlot::PopStyleColor();
		ImPlot::EndPlot();
	}
}

void GuiPass::ParameterTab() noexcept
{
	auto paramsView = App::GetParams();
	Span<ParamVariant> params = paramsView.Variable();
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

		const char* l1 = p.GetGroup();
		size_t n1 = strlen(l1);
		memcpy(currGroup, p.GetGroup(), n1);
		currGroup[n1] = '\0';

		size_t i = currGroupIdx;
		while (i < params.size() && strcmp(params[i].GetGroup(), currGroup) == 0)
			i++;

		if (ImGui::TreeNode(p.GetGroup()))
		{
			for (size_t currSubgroupIdx = currGroupIdx; currSubgroupIdx < i;)
			{
				ParamVariant& currParam = params[currSubgroupIdx];

				const char* l2 = currParam.GetSubGroup();
				size_t n2 = strlen(l2);
				memcpy(currSubGroup, currParam.GetSubGroup(), n2);
				currSubGroup[n2] = '\0';

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
	if (App::GetTimer().GetTotalFrameCount() % 5 == 0)
	{
		auto timings = App::GetRenderer().GetGpuTimer().GetFrameTimings();
		m_cachedTimings.clear();
		m_cachedTimings.append_range(timings.begin(), timings.end());

		if(m_cachedTimings.size() > 0)
		{
			std::sort(m_cachedTimings.begin(), m_cachedTimings.begin() + m_cachedTimings.size(),
				[](const GpuTimer::Timing& t0, const GpuTimer::Timing& t1)
				{
					return strcmp(t0.Name, t1.Name) < 0;
				});
		}
	}

	if (m_cachedTimings.empty())
		return;

	const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg | 
		ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Hideable;

	// When using ScrollX or ScrollY we need to specify a size for our table container!
	// Otherwise by default the table will fit all available space, like a BeginChild() call.
	const float TEXT_BASE_HEIGHT = ImGui::GetTextLineHeightWithSpacing();
	ImVec2 outer_size = ImVec2(0.0f, TEXT_BASE_HEIGHT * 11);
	if (ImGui::BeginTable("table_scrolly", 2, flags, outer_size))
	{
		ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible

		ImGui::TableSetupColumn("RenderPass", ImGuiTableColumnFlags_None);
		ImGui::TableSetupColumn("Delta (ms)", ImGuiTableColumnFlags_None);
		ImGui::TableHeadersRow();

		ImU32 row_bg_color = ImGui::GetColorU32(ImVec4(7.0f / 255, 26.0f / 255, 56.0f / 255, 1.0f));
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0 + 0, row_bg_color);

		for (int row = 0; row < (int)m_cachedTimings.size(); row++)
		{
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", m_cachedTimings[row].Name);

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", (float)m_cachedTimings[row].Delta);
		}

		ImGui::EndTable();
	}
}

void GuiPass::ShaderReloadTab() noexcept
{
	auto reloadHandlers = App::GetShaderReloadHandlers();
	Span<App::ShaderReloadHandler> handlers = reloadHandlers.Variable();

	ImGui::Text("Select a shader to reload");

	// TODO m_currShader becomes invalid when there's been a change in reloadHandlers 
	if (ImGui::BeginCombo("Shader", m_currShader >= 0 ? handlers[m_currShader].Name : "None", 0))
	{
		int i = 0;

		for (auto& handler : handlers)
		{
			bool selected = (m_currShader == i);
			if (ImGui::Selectable(handler.Name, selected))
				m_currShader = i;

			if(selected)
				ImGui::SetItemDefaultFocus();

			i++;
		}

		ImGui::EndCombo();
	}

	if(m_currShader == -1)
		ImGui::BeginDisabled();

	if (ImGui::Button("Reload"))
		handlers[m_currShader].Dlg();

	if (m_currShader == -1)
		ImGui::EndDisabled();
}

void GuiPass::RebuildFontTex() noexcept
{
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->GetTexDataAsRGBA32(&m_font.Pixels, &m_font.Width, &m_font.Height);

	m_font.IsStale = true;
}
