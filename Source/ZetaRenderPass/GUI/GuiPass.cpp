#include "GuiPass.h"
#include <Math/Vector.h>
#include <Core/RendererCore.h>
#include <Core/CommandList.h>
#include <Support/Param.h>
#include <Support/Stat.h>
#include <Scene/SceneCore.h>
#include <Scene/Camera.h>
#include <App/Timer.h>
#include <Utility/SynchronizedView.h>

#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <algorithm>

#include "../Assets/Font/IconsFontAwesome6.h"

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Core::Direct3DUtil;
using namespace ZetaRay::Math;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Scene;

namespace
{
    void AddParamRange(MutableSpan<ParamVariant> params, size_t offset, size_t count)
    {
        std::sort(params.begin() + offset, params.begin() + offset + count, [](ParamVariant& p1, ParamVariant& p2)
            {
                return strcmp(p1.GetName(), p2.GetName()) < 0;
            });

        for (size_t p = offset; p < offset + count; p++)
        {
            ParamVariant& param = params[p];

            if (param.GetType() == PARAM_TYPE::PT_enum)
            {
                auto& fp = param.GetEnum();
                int idx = fp.m_curr;
                if(ImGui::Combo(param.GetName(), &idx, fp.m_values, fp.m_num))
                    param.SetEnum(idx);
            }
            else if (param.GetType() == PARAM_TYPE::PT_float)
            {
                auto& fp = param.GetFloat();
                float v = fp.m_value;

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
                int v = ip.m_value;

                if (ImGui::SliderInt(param.GetName(), &v, ip.m_min, ip.m_max))
                    param.SetInt(v);
            }
            else if (param.GetType() == PARAM_TYPE::PT_float2)
            {
                auto& fp = param.GetFloat2();
                float2 v = fp.m_value;

                if (ImGui::SliderFloat2(param.GetName(), reinterpret_cast<float*>(&v), fp.m_min, fp.m_max, "%.2f"))
                    param.SetFloat2(v);
            }            
            else if (param.GetType() == PARAM_TYPE::PT_float3)
            {
                auto& fp = param.GetFloat3();
                float3 v = fp.m_value;

                if (ImGui::SliderFloat3(param.GetName(), reinterpret_cast<float*>(&v), fp.m_min, fp.m_max, "%.2f"))
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
                float3 v = fp.m_value;

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

    void DrawAxis(const float3& pos, const float3& xAxis, const float3& zAxis, const float3& xColor,
        const float3& zColor, float lineWidth)
    {
        // axis
        float axis_x[2];
        float axis_y[2];

        float arrow_x[3];
        float arrow_y[3];

        // arrow tip
        constexpr float arrowLenX = 0.25f;
        constexpr float arrowLenY = 0.25f;

        // rotate and plit
        auto func = [&](const float3& color)
            {
                float2 rotMatCol1 = float2(xAxis.x, xAxis.z);
                float2 rotMatCol2 = float2(zAxis.x, zAxis.z);

                float2 rotated = arrow_x[0] * rotMatCol1 + arrow_y[0] * rotMatCol2;
                arrow_x[0] = pos.x + rotated.x;
                arrow_y[0] = pos.z + rotated.y;

                rotated = arrow_x[2] * rotMatCol1 + arrow_y[2] * rotMatCol2;
                arrow_x[2] = pos.x + rotated.x;
                arrow_y[2] = pos.z + rotated.y;

                ImPlot::SetNextLineStyle(ImVec4(color.x, color.y, color.z, 1.0f), lineWidth);
                ImPlot::PlotLine("", arrow_x, arrow_y, ZetaArrayLen(arrow_x));
            };

        // starting point
        axis_x[0] = pos.x;
        axis_y[0] = pos.z;

        // end point
        axis_x[1] = pos.x + zAxis.x;
        axis_y[1] = pos.z + zAxis.z;

        ImPlot::SetNextLineStyle(ImVec4(zColor.x, zColor.y, zColor.z, 1.0f), lineWidth);
        ImPlot::PlotLine("Z", axis_x, axis_y, ZetaArrayLen(axis_x));

        // Z axis
        // starting point
        arrow_x[0] = 0.0f - arrowLenX;
        arrow_y[0] = 1.0f - arrowLenY;
        // middle point
        arrow_x[1] = axis_x[1];
        arrow_y[1] = axis_y[1];
        // end point
        arrow_x[2] = 0.0f + arrowLenX;
        arrow_y[2] = 1.0f - arrowLenY;

        func(zColor);

        // X axis
        // end point
        axis_x[1] = pos.x + xAxis.x;
        axis_y[1] = pos.z + xAxis.z;
        ImPlot::SetNextLineStyle(ImVec4(xColor.x, xColor.y, xColor.z, 1.0f), lineWidth);
        ImPlot::PlotLine("X", axis_x, axis_y, ZetaArrayLen(axis_x));

        // starting point
        arrow_x[0] = 1.0f - arrowLenX;
        arrow_y[0] = 0.0f + arrowLenY;
        // middle point
        arrow_x[1] = axis_x[1];
        arrow_y[1] = axis_y[1];
        // end point
        arrow_x[2] = 1.0f - arrowLenX;
        arrow_y[2] = 0.0f - arrowLenY;

        func(xColor);
    }

    void ShowStyles()
    {
        if (ImGui::BeginTabItem("Colors"))
        {
            ImGuiStyle& style = ImGui::GetStyle();

            static int output_dest = 0;
            static bool output_only_modified = true;

            static ImGuiTextFilter filter;
            filter.Draw("Filter colors", ImGui::GetFontSize() * 16);

            static ImGuiColorEditFlags alpha_flags = 0;

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
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sizes"))
        {
            ImGuiStyle& style = ImGui::GetStyle();

            ImGui::Text("Main");
            ImGui::SliderFloat2("WindowPadding", (float*)&style.WindowPadding, 0.0f, 20.0f, "%.0f");
            ImGui::SliderFloat2("FramePadding", (float*)&style.FramePadding, 0.0f, 20.0f, "%.0f");
            ImGui::SliderFloat2("CellPadding", (float*)&style.CellPadding, 0.0f, 20.0f, "%.0f");
            ImGui::SliderFloat2("ItemSpacing", (float*)&style.ItemSpacing, 0.0f, 20.0f, "%.0f");
            ImGui::SliderFloat2("ItemInnerSpacing", (float*)&style.ItemInnerSpacing, 0.0f, 20.0f, "%.0f");
            ImGui::SliderFloat2("TouchExtraPadding", (float*)&style.TouchExtraPadding, 0.0f, 10.0f, "%.0f");
            ImGui::SliderFloat("IndentSpacing", &style.IndentSpacing, 0.0f, 30.0f, "%.0f");
            ImGui::SliderFloat("ScrollbarSize", &style.ScrollbarSize, 1.0f, 20.0f, "%.0f");
            ImGui::SliderFloat("GrabMinSize", &style.GrabMinSize, 1.0f, 20.0f, "%.0f");
            ImGui::Text("Borders");
            ImGui::SliderFloat("WindowBorderSize", &style.WindowBorderSize, 0.0f, 1.0f, "%.0f");
            ImGui::SliderFloat("ChildBorderSize", &style.ChildBorderSize, 0.0f, 1.0f, "%.0f");
            ImGui::SliderFloat("PopupBorderSize", &style.PopupBorderSize, 0.0f, 1.0f, "%.0f");
            ImGui::SliderFloat("FrameBorderSize", &style.FrameBorderSize, 0.0f, 1.0f, "%.0f");
            ImGui::SliderFloat("TabBorderSize", &style.TabBorderSize, 0.0f, 1.0f, "%.0f");
            ImGui::Text("Rounding");
            ImGui::SliderFloat("WindowRounding", &style.WindowRounding, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("ChildRounding", &style.ChildRounding, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("FrameRounding", &style.FrameRounding, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("PopupRounding", &style.PopupRounding, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("ScrollbarRounding", &style.ScrollbarRounding, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("GrabRounding", &style.GrabRounding, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("LogSliderDeadzone", &style.LogSliderDeadzone, 0.0f, 12.0f, "%.0f");
            ImGui::SliderFloat("TabRounding", &style.TabRounding, 0.0f, 12.0f, "%.0f");
            ImGui::Text("Alignment");
            ImGui::SliderFloat2("WindowTitleAlign", (float*)&style.WindowTitleAlign, 0.0f, 1.0f, "%.2f");
            int window_menu_button_position = style.WindowMenuButtonPosition + 1;
            ImGui::Combo("ColorButtonPosition", (int*)&style.ColorButtonPosition, "Left\0Right\0");
            ImGui::SliderFloat2("ButtonTextAlign", (float*)&style.ButtonTextAlign, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat2("SelectableTextAlign", (float*)&style.SelectableTextAlign, 0.0f, 1.0f, "%.2f");
            ImGui::Text("Safe Area Padding");
            ImGui::SliderFloat2("DisplaySafeAreaPadding", (float*)&style.DisplaySafeAreaPadding, 0.0f, 30.0f, "%.0f");

            ImGui::EndTabItem();
        }
    }
}

//--------------------------------------------------------------------------------------
// GuiPass
//--------------------------------------------------------------------------------------

GuiPass::GuiPass()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{}

GuiPass::~GuiPass()
{
    Reset();
}

void GuiPass::Init()
{
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsResizeFromEdges = true;

    // root signature
    {
        // root constants
        m_rootSig.InitAsConstants(0,
            sizeof(cbGuiPass) / sizeof(DWORD),
            0);

        D3D12_ROOT_SIGNATURE_FLAGS flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        auto samplers = App::GetRenderer().GetStaticSamplers();
        RenderPassBase::InitRenderPass("GuiPass", flags, samplers);
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

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DUtil::GetPSODesc(&inputLayout,
            1,
            rtv);

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
        m_pso = m_psoLib.GetGraphicsPSO(0, psoDesc, m_rootSigObj.Get(), COMPILED_VS[0], COMPILED_PS[0]);
    }
}

void GuiPass::Reset()
{
    if (IsInitialized())
    {
        for (int i = 0; i < Constants::NUM_BACK_BUFFERS; i++)
        {
            m_imguiFrameBuffs[i].IndexBuffer.Reset();
            m_imguiFrameBuffs[i].VertexBuffer.Reset();
        }

        m_cachedTimings.free_memory();

        RenderPassBase::ResetRenderPass();
    }
}

void GuiPass::UpdateBuffers()
{
    ImDrawData* draw_data = ImGui::GetDrawData();
    const int currOutIdx = App::GetRenderer().GetCurrentBackBufferIndex();

    // Avoid rendering when minimized
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    auto& fr = m_imguiFrameBuffs[currOutIdx];

    // Create and grow vertex/index buffers if needed
    if (!fr.VertexBuffer.IsInitialized() || fr.NumVertices < draw_data->TotalVtxCount)
    {
        fr.NumVertices = draw_data->TotalVtxCount + 5000;
        fr.VertexBuffer = GpuMemory::GetUploadHeapBuffer(fr.NumVertices * sizeof(ImDrawVert));
    }

    // Upload vertex data into a single contiguous GPU buffer
    uint32_t offset = 0;

    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        fr.VertexBuffer.Copy(offset, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert), cmd_list->VtxBuffer.Data);
        offset += cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
    }

    if (!fr.IndexBuffer.IsInitialized() || fr.NumIndices < draw_data->TotalIdxCount)
    {
        //SafeRelease(fr->IndexBuffer);
        fr.NumIndices = draw_data->TotalIdxCount + 10000;
        fr.IndexBuffer = GpuMemory::GetUploadHeapBuffer(fr.NumIndices * sizeof(ImDrawIdx));
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

void GuiPass::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT, "Invalid downcast");
    GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(cmdList);

    auto& renderer = App::GetRenderer(); 
    auto& gpuTimer = renderer.GetGpuTimer();

    directCmdList.PIXBeginEvent("ImGui");
    const uint32_t queryIdx = gpuTimer.BeginQuery(directCmdList, "ImGui");

    directCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());
    directCmdList.SetPipelineState(m_pso);

    RenderSettings();
    RenderMainHeader();
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

    void* userData = ImGui::GetIO().UserData;
    memcpy(&cb.FontTex, &userData, sizeof(cb.FontTex));

    D3D12_VIEWPORT viewports[1] = { renderer.GetDisplayViewport() };
    directCmdList.RSSetViewports(1, viewports);

    m_rootSig.SetRootConstants(0, sizeof(cb) / sizeof(DWORD), &cb);
    m_rootSig.End(directCmdList);

    // Bind shader and vertex buffers
    unsigned int stride = sizeof(ImDrawVert);
    unsigned int offset = 0;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = fr.VertexBuffer.GpuVA() + offset;
    vbv.SizeInBytes = fr.NumVertices * stride;
    vbv.StrideInBytes = stride;

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = fr.IndexBuffer.GpuVA();
    ibv.SizeInBytes = fr.NumIndices * sizeof(ImDrawIdx);
    ibv.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    directCmdList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    directCmdList.IASetVertexAndIndexBuffers(vbv, ibv);

    Assert(m_cpuDescriptors[SHADER_IN_CPU_DESC::RTV].ptr > 0, "RTV hasn't been set.");
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

    gpuTimer.EndQuery(directCmdList, queryIdx);

    // HACK this is the last RenderPass, transition to PRESENT can be done here
    directCmdList.ResourceBarrier(renderer.GetCurrentBackBuffer().Resource(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);

    directCmdList.PIXEndEvent();
}

void GuiPass::RenderSettings()
{
    const int displayWidth = App::GetRenderer().GetDisplayWidth();
    const int displayHeight = App::GetRenderer().GetDisplayHeight();
    const float wndPosX = ceilf(displayWidth * (1 - m_dbgWndWidthPct));

    ImGui::Begin("Debug Window", nullptr, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowPos(ImVec2(wndPosX, 0.0f), ImGuiCond_Once);
    ImGui::SetWindowSize(ImVec2(m_dbgWndWidthPct * displayWidth, m_dbgWndHeightPct * displayHeight), 
        ImGuiCond_Once);

    m_logWndWidth = displayWidth - ImGui::GetWindowWidth();

    if (ImGui::CollapsingHeader(ICON_FA_INFO "  Info", ImGuiTreeNodeFlags_None))
    {
        InfoTab();
        ImGui::Text("");
    }

    if (ImGui::CollapsingHeader(ICON_FA_CAMERA "  Camera", ImGuiTreeNodeFlags_None))
    {
        CameraTab();
        ImGui::Text("");
    }

    if (ImGui::CollapsingHeader(ICON_FA_WRENCH "  Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ParameterTab();
        ImGui::Text("");
    }

    //if (ImGui::CollapsingHeader("Style", ImGuiTreeNodeFlags_None))
    //{
    //    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None))
    //    {
    //        ShowStyles();
    //        ImGui::EndTabBar();
    //    }

    //    ImGui::Text("");
    //}

    if (ImGui::CollapsingHeader(ICON_FA_ROTATE_RIGHT "  Shader Hot-Reload", ImGuiTreeNodeFlags_None))
    {
        ShaderReloadTab();
        ImGui::Text("");
    }

    RenderProfiler();

    ImGui::End();
}

void GuiPass::RenderProfiler()
{
    const float w = ImGui::GetWindowWidth();

    auto& renderer = App::GetRenderer();
    auto& timer = App::GetTimer();

    if (ImGui::CollapsingHeader(ICON_FA_CHART_LINE "  Stats", ImGuiTreeNodeFlags_None))
    {
        ImGui::Text("Frame %llu", timer.GetTotalFrameCount());

        ImGui::SameLine();
        ImGui::Text("        ");
        ImGui::SameLine();

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
                ImGui::Text("\t%s: %.2f", s.GetName(), s.GetFloat());
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

    if (ImGui::CollapsingHeader(ICON_FA_CLOCK "  GPU Timings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto frameTimeHist = App::GetFrameTimeHistory();

        float max_ = 0.0f;
        for (auto f : frameTimeHist)
            max_ = Math::Max(max_, f);

        if (ImPlot::BeginPlot("Frame Time", ImVec2(w * 0.9f, 150.0f), ImPlotFlags_NoLegend))
        {
            ImPlot::SetupAxes("Moving Window", "Time (ms)", 0, ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxesLimits(0, (double)frameTimeHist.size(), 0, max_ + 1.0, ImGuiCond_Always);
            //ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle);
            ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(85 / 255.0f, 85 / 255.0f, 85 / 255.0f, 1.0f));

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

void GuiPass::RenderLogWindow()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.014286487, 0.014286487, 0.0142864870, 0.995f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));

    const float h_prev = ImGui::GetWindowSize().y;

    ImGui::Begin("Logs", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);

    const int displayHeight = App::GetRenderer().GetDisplayHeight();
    const float logWndHeightPct = m_logWndHeightPct;

    ImGui::SetWindowSize(ImVec2(m_logWndWidth, logWndHeightPct * displayHeight), ImGuiCond_Once);
    ImGui::SetWindowPos(ImVec2(0, h_prev), ImGuiCond_Always);

    ImGui::Text("#Items: %d\t", (int)m_logs.size());
    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_TRASH_CAN "  Clear"))
        m_logs.clear();

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_XMARK "  Close"))
        m_closeLogsTab = true;

    ImGui::Separator();

    ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiChildFlags_AlwaysUseWindowPadding);

    char* buf;
    char* buf_end;

    // TODO consider using ImGuiListClipper
    for (auto& msg : m_logs)
    {
        ImVec4 color = msg.Type == App::LogMessage::INFO ? ImVec4(0.3f, 0.4f, 0.5f, 1.0f) : ImVec4(0.4f, 0.2f, 0.2f, 1.0f);
        ImGui::TextColored(color, msg.Msg);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    ImGui::End();
}

void GuiPass::RenderMainHeader()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Tab, style.Colors[ImGuiCol_WindowBg]);
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.0295568369f, 0.0295568369f, 0.0295568369f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.05098039215f, 0.05490196078f, 0.05490196078f, 1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(15, style.ItemInnerSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.WindowPadding.y));

    const int displayHeight = App::GetRenderer().GetDisplayHeight();
    const float wndHeight = m_headerWndHeightPct * displayHeight;

    ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | 
        ImGuiWindowFlags_NoResize);

    ImGui::SetWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetWindowSize(ImVec2(m_logWndWidth, wndHeight), ImGuiCond_Always);

    ImGui::Text("        ");
    ImGui::SameLine();
    ImGui::BeginTabBar("Header", ImGuiTabBarFlags_None);

    auto flags = m_closeLogsTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    const bool showMainWnd = ImGui::BeginTabItem(ICON_FA_DISPLAY "        Main        ", nullptr, flags);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone))
    {
        ImGui::PopStyleVar();
        ImGui::SetTooltip("Scene View");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.WindowPadding.y));
    }

    if(showMainWnd)
    {
        ImGui::SetWindowSize(ImVec2(m_logWndWidth, m_headerWndHeightPct * displayHeight), ImGuiCond_Always);
        ImGui::EndTabItem();
    }

    const bool renderGraphTab = ImGui::BeginTabItem(ICON_FA_SHARE_NODES "        Render Graph        ");

    if (ImGui::IsItemHovered())
    {
        ImGui::PopStyleVar();
        ImGui::SetTooltip("Render Graph Visualization (Use RMB for Panning).");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.WindowPadding.y));
    }

    if(renderGraphTab)
    {
        const float h_prev = ImGui::GetWindowHeight();

        ImGui::Begin(" ", nullptr, ImGuiWindowFlags_NoMove);

        ImGui::SetWindowSize(ImVec2(m_logWndWidth, displayHeight - h_prev), ImGuiCond_Once);
        ImGui::SetWindowPos(ImVec2(0, h_prev), ImGuiCond_Always);

        App::GetScene().DebugDrawRenderGraph();

        ImGui::End();

        ImGui::EndTabItem();
    }

    // Append the latest log messages
    auto frameLogs = App::GetFrameLogs().Variable();
    m_prevNumLogs = (int)m_logs.size();
    m_logs.append_range(frameLogs.begin(), frameLogs.end());

    flags = ImGuiTabItemFlags_None;
    if (!m_firstTime && m_logs.size() != m_prevNumLogs)
        flags = ImGuiTabItemFlags_SetSelected;

    if(ImGui::BeginTabItem(ICON_FA_TERMINAL "        Logs        ", nullptr, flags))
    {
        m_closeLogsTab = false;
        RenderLogWindow();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);

    ImGui::End();

    m_firstTime = false;
}

void GuiPass::InfoTab()
{
    auto& renderer = App::GetRenderer();
    ImGui::Text(" - Device: %s", renderer.GetDeviceDescription());
    ImGui::Text(" - Render Resolution: %d x %d", renderer.GetRenderWidth(), renderer.GetRenderHeight());
    ImGui::Text(" - Display Resolution: %d x %d (%u DPI)", renderer.GetDisplayWidth(), renderer.GetDisplayHeight(), App::GetDPI());
    ImGui::Text("");
    ImGui::Text(" - Controls:");
    ImGui::Text("\t- WASD+LMB moves the camera");
    ImGui::Text("\t- MMB zooms in/out");
}

void GuiPass::CameraTab()
{
    const Camera& camera = App::GetCamera();
    float3 camPos = camera.GetPos();
    float3 viewBasisX = camera.GetBasisX();
    float3 viewBasisY = camera.GetBasisY();
    float3 viewBasisZ = camera.GetBasisZ();

    ImGui::Text(" - Camera Position: (%.3f, %.3f, %.3f)", camPos.x, camPos.y, camPos.z);
    ImGui::SameLine(275);
    if (ImGui::Button(ICON_FA_COPY " Copy##0"))
    {
        StackStr(buffer, n, "%.4f, %.4f, %.4f", camPos.x, camPos.y, camPos.z);
        App::CopyToClipboard(buffer);
    }
    ImGui::SetItemTooltip("Copy vector to clipboard");

    ImGui::Text(" - View Basis X: (%.3f, %.3f, %.3f)", viewBasisX.x, viewBasisX.y, viewBasisX.z);
    ImGui::Text(" - View Basis Y: (%.3f, %.3f, %.3f)", viewBasisY.x, viewBasisY.y, viewBasisY.z);
    ImGui::Text(" - View Basis Z: (%.3f, %.3f, %.3f)", viewBasisZ.x, viewBasisZ.y, viewBasisZ.z);
    ImGui::SameLine(275);
    if (ImGui::Button(ICON_FA_COPY " Copy##1"))
    {
        StackStr(buffer, n, "%.4f, %.4f, %.4f", viewBasisZ.x, viewBasisZ.y, viewBasisZ.z);
        App::CopyToClipboard(buffer);
    }
    ImGui::SetItemTooltip("Copy vector to clipboard");

    ImGui::Text(" - Aspect Ratio: %f", camera.GetAspectRatio());
    ImGui::Text(" - Near Plane Z: %.3f", camera.GetNearZ());

    constexpr int plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_Equal;

    if (ImPlot::BeginPlot("Camera Coodinate System", ImVec2(250.0f, 250.0f), plotFlags))
    {
        const float3 pos = camera.GetPos();
        constexpr int axisFlags = ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoHighlight;
        ImPlot::SetupAxes("X", "Z", axisFlags, axisFlags);
        ImPlot::SetupAxesLimits(pos.x - 3, pos.x + 3, pos.z - 3, pos.z + 3, ImGuiCond_Always);

        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
        const auto wndCol = colors[ImGuiCol_WindowBg];
        ImPlot::PushStyleColor(ImPlotCol_FrameBg, wndCol);

        const float3 xAxis = camera.GetBasisX();
        const float3 zAxis = camera.GetBasisZ();
        DrawAxis(pos, xAxis, zAxis, float3(0.99f, 0.15f, 0.05f), float3(0.1f, 0.5f, 0.99f), 3.0f);

        ImPlot::PopStyleColor();
        ImPlot::EndPlot();
    }
}

void GuiPass::ParameterTab()
{
    auto paramsView = App::GetParams();
    MutableSpan<ParamVariant> params = paramsView.Variable();
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
            memcpy(currGroup, p.GetGroup(), ParamVariant::MAX_GROUP_LEN);

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

void GuiPass::GpuTimingsTab()
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

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | 
        ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | 
        ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

    // When using ScrollX or ScrollY we need to specify a size for our table container!
    // Otherwise by default the table will fit all available space, like a BeginChild() call.
    const float TEXT_BASE_HEIGHT = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 outer_size = ImVec2(0, TEXT_BASE_HEIGHT * 11);
    if (ImGui::BeginTable("table_scrolly", 2, flags, outer_size))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible

        ImGui::TableSetupColumn("\t\tRender Pass", ImGuiTableColumnFlags_None);
        ImGui::TableSetupColumn("\t\tDelta (ms)", ImGuiTableColumnFlags_None);
        ImGui::TableHeadersRow();

        for (int row = 0; row < (int)m_cachedTimings.size(); row++)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text(" %s", m_cachedTimings[row].Name);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("\t\t\t%.3f", (float)m_cachedTimings[row].Delta);
        }

        ImGui::EndTable();
    }
}

void GuiPass::ShaderReloadTab()
{
    auto reloadHandlers = App::GetShaderReloadHandlers();
    MutableSpan<App::ShaderReloadHandler> handlers = reloadHandlers.Variable();

    if (!handlers.empty())
    {
        std::sort(handlers.begin(), handlers.end(), [](const App::ShaderReloadHandler& lhs, const App::ShaderReloadHandler& rhs)
            {
                return strcmp(lhs.Name, rhs.Name) < 0;
            });
    }

    ImGui::Text("Select a shader to reload:");

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

