#include "GuiPass.h"
#include <Core/CommandList.h>
#include <Support/Param.h>
#include <Support/Stat.h>
#include <Scene/SceneCore.h>
#include <Scene/Camera.h>
#include <App/Timer.h>
#include <Utility/SynchronizedView.h>
#include <Math/CollisionFuncs.h>
#include <Math/Quaternion.h>

#include <ImGui/imgui.h>
#include <ImGui/implot.h>
#include <ImGui/ImGuizmo.h>
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
using namespace ZetaRay::Model;

namespace
{
    void AddParamRange(MutableSpan<ParamVariant> params, int offset, int count)
    {
        // Sort by name among current subgroup
        std::sort(params.begin() + offset, params.begin() + offset + count, 
            [](const ParamVariant& p1, const ParamVariant& p2)
            {
                return strcmp(p1.GetName(), p2.GetName()) < 0;
            });

        for (int p = offset; p < offset + count; p++)
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

                auto flags = (int)ImGuiSliderFlags_None;
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

        // rotate and plot
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

void GuiPass::Init()
{
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsResizeFromEdges = true;

    // Root signature
    {
        m_rootSig.InitAsConstants(0, sizeof(cbGuiPass) / sizeof(DWORD), 0);

        constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
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
            1, rtv);

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

        m_psoLib.CompileGraphicsPSO(0, psoDesc, m_rootSigObj.Get(), COMPILED_VS[0], COMPILED_PS[0]);
    }

    auto* ctx = ImGui::GetCurrentContext();
    ImGuizmo::SetImGuiContext(ctx);
    ImGuizmo::AllowAxisFlip(false);
}

void GuiPass::OnWindowResized()
{
    m_appWndSizeChanged = true;
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
        fr.VertexBuffer.Copy(offset, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert), 
            cmd_list->VtxBuffer.Data);
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
        fr.IndexBuffer.Copy(offset, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx), 
            cmd_list->IdxBuffer.Data);
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
    directCmdList.SetPipelineState(m_psoLib.GetPSO(0));

    RenderUI();

    ImGui::Render();
    UpdateBuffers();

    const int currBackBuffIdx = renderer.GetCurrentBackBufferIndex();

    // Rendering
    ImDrawData* draw_data = ImGui::GetDrawData();
    auto& fr = m_imguiFrameBuffs[currBackBuffIdx];

    // Setup desired DX state
    // Setup orthographic projection matrix into our constant buffer
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to 
    // draw_data->DisplayPos+data_data->DisplaySize (bottom right).
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
    directCmdList.ResourceBarrier(const_cast<Texture&>(renderer.GetCurrentBackBuffer()).Resource(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);

    directCmdList.PIXEndEvent();
}

void GuiPass::RenderUI()
{
    ImGuizmo::BeginFrame();

    RenderToolbar();

    if (!m_hideUI)
    {
        SceneCore& scene = App::GetScene();
        TriangleMesh instanceMesh;
        float4x4a W;
        uint64_t firstPicked = Scene::INVALID_INSTANCE;

        if (auto picks = scene.GetPickedInstances(); !picks.m_span.empty())
        {
            firstPicked = picks.m_span[0];

            W = float4x4a(scene.GetToWorld(firstPicked));
            instanceMesh = *scene.GetInstanceMesh(firstPicked).value();

            if (m_gizmoActive)
                RenderGizmo(picks.m_span, instanceMesh, W);
        }

        RenderSettings(firstPicked, instanceMesh, W);
        RenderMainHeader();
    }

    m_firstTime = false;
    m_appWndSizeChanged = false;
}

void GuiPass::RenderToolbar()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(0.09521219013259, 0.09521219013259, 0.09521219013259, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        ImVec4(0.0630100295, 0.168269396, 0.45078584552, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4(0.039556837, 0.039556837, 0.039556837, 0.87f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1, 1));

    ImGui::SetNextWindowPos(ImVec2((float)5.0f, m_headerWndHeight + 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(60.0f, 250.0f), ImGuiCond_Always);

    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground);

    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 1));

    if (!m_hideUI)
    {
        const bool wasGizmoActive = m_gizmoActive;

        if (!wasGizmoActive)
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);

        if (ImGui::Button(ICON_FA_ARROW_POINTER "##65", ImVec2(40.0f, 40.0f)))
            m_gizmoActive = !m_gizmoActive;
        ImGui::SetItemTooltip("Select");

        if (!wasGizmoActive)
            ImGui::PopStyleColor();

        const bool isTranslation = m_gizmoActive && (m_currGizmoOperation == ImGuizmo::OPERATION::TRANSLATE);
        const bool isRotation = m_gizmoActive && (m_currGizmoOperation == ImGuizmo::OPERATION::ROTATE);
        const bool isScale = m_gizmoActive && (m_currGizmoOperation == ImGuizmo::OPERATION::SCALE);

        if (isTranslation)
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);

        if (ImGui::Button(ICON_FA_UP_DOWN_LEFT_RIGHT "##3", ImVec2(40.0f, 40.0f)))
        {
            m_currGizmoOperation = ImGuizmo::OPERATION::TRANSLATE;
            m_gizmoActive = true;
        }
        ImGui::SetItemTooltip("Move (G)");

        if (isTranslation)
            ImGui::PopStyleColor();

        if (isRotation)
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);

        if (ImGui::Button(ICON_FA_ARROWS_ROTATE "##4", ImVec2(40.0f, 40.0f)))
        {
            m_currGizmoOperation = ImGuizmo::OPERATION::ROTATE;
            m_gizmoActive = true;
        }
        ImGui::SetItemTooltip("Rotate (R)");

        if (isRotation)
            ImGui::PopStyleColor();

        if (isScale)
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 2));

        if (ImGui::Button(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE "##5", ImVec2(40.0f, 40.0f)))
        {
            m_currGizmoOperation = ImGuizmo::OPERATION::SCALE;
            m_gizmoActive = true;
        }
        ImGui::SetItemTooltip("Scale (S)");

        if (isScale)
            ImGui::PopStyleColor();

        ImGui::PopStyleVar();

        if (ImGui::IsKeyPressed(ImGuiKey_G))
        {
            m_currGizmoOperation = ImGuizmo::TRANSLATE;
            m_gizmoActive = true;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_R))
        {
            m_currGizmoOperation = ImGuizmo::ROTATE;
            m_gizmoActive = true;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_C))
        {
            m_currGizmoOperation = ImGuizmo::SCALE;
            m_gizmoActive = true;
        }
    }

    const char* icon = !m_hideUI ? ICON_FA_TOGGLE_ON "##1" : ICON_FA_TOGGLE_OFF "##1";
    if (ImGui::Button(icon, ImVec2(40.0f, 40.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_H))
        m_hideUI = !m_hideUI;
    ImGui::SetItemTooltip("Show/Hide UI (H)");

    if (ImGui::Button(ICON_FA_CAMERA_RETRO "##2", ImVec2(40.0f, 40.0f)))
        App::GetScene().CaptureScreen();
    ImGui::SetItemTooltip("Take Screenshot");

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    ImGui::End();
}

void GuiPass::RenderSettings(uint64 pickedID, const TriangleMesh& mesh, const float4x4a& W)
{ 
    const int displayWidth = App::GetRenderer().GetDisplayWidth();
    const int displayHeight = App::GetRenderer().GetDisplayHeight();

    // Round to nearest
    const int wndSizeX = (int)std::fmaf((float)displayWidth, m_dbgWndWidthPct, 0.5f);
    const int wndSizeY = (int)std::fmaf((float)displayHeight, m_dbgWndHeightPct, 0.5f);
    const int wndPosX = displayWidth - wndSizeX;
    m_headerWndWidth = wndPosX;

    ImGui::SetNextWindowPos(ImVec2((float)wndPosX, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)wndSizeX, (float)wndSizeY), ImGuiCond_Always);
    // Hide resize grip
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, 0);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, style.WindowPadding.y));

    if (ImGui::Begin(ICON_FA_WRENCH " Settings", nullptr, ImGuiWindowFlags_HorizontalScrollbar |
        ImGuiWindowFlags_NoMove))
    {
        m_dbgWndWidthPct = ImGui::GetWindowWidth() / (float)displayWidth;
        m_dbgWndHeightPct = !ImGui::IsWindowCollapsed() ? ImGui::GetWindowHeight() / (float)displayHeight :
            m_dbgWndHeightPct;

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

        {
            ParameterTab();
        }

        if (pickedID != Scene::INVALID_INSTANCE)
        {
            if (ImGui::CollapsingHeader(ICON_FA_CUBE "  Object"))
            {
                PickedWorldTransform(pickedID, mesh, W);
                ImGui::Text("");
            }

            if (ImGui::CollapsingHeader(ICON_FA_PALETTE "  Material"))
            {
                PickedMaterial(pickedID);
                ImGui::Text("");
            }
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
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::End();
}

void GuiPass::RenderProfiler()
{
    auto& timer = App::GetTimer();
    auto& scene = App::GetScene();;

    if (ImGui::CollapsingHeader(ICON_FA_CHART_LINE "  Stats", ImGuiTreeNodeFlags_None))
    {
        ImGui::Text("Frame %llu", timer.GetTotalFrameCount());
        ImGui::SeparatorText("Performance");

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

        for (auto s : App::GetStats().m_span)
            func(s);

        ImGui::SeparatorText("Scene");
        ImGui::Text("\t#Instances: %u", (uint32_t)scene.TotalNumInstances());
        ImGui::Text("\t#Meshes: %u", (uint32_t)scene.TotalNumMeshes());
        ImGui::Text("\t#Triangles: %u", (uint32_t)scene.TotalNumTriangles());
        ImGui::Text("\t#Materials: %u", (uint32_t)scene.TotalNumMaterials());
        ImGui::Text("\t#Emissive Instances: %u", (uint32_t)scene.NumEmissiveInstances());
        ImGui::Text("\t#Emissive Triangles: %u", (uint32_t)scene.NumEmissiveTriangles());

        ImGui::Text("");
    }

    if (ImGui::CollapsingHeader(ICON_FA_CLOCK "  GPU Timings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto frameTimeHist = App::GetFrameTimeHistory();
        const float w = ImGui::GetWindowWidth();

        float maxTime = 0.0f;
        for (auto f : frameTimeHist)
            maxTime = Math::Max(maxTime, f);

        if (ImPlot::BeginPlot("Frame Time", ImVec2(w * m_frameHistWidthPct, 150.0f), ImPlotFlags_NoLegend))
        {
            ImPlot::SetupAxes("Moving Window", "Time (ms)", 0, ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxesLimits(0, (double)frameTimeHist.size(), 0, maxTime + 1.0, ImGuiCond_Always);
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
    // Hide resize grip
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, 0);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, 0);

    const int displayHeight = App::GetRenderer().GetDisplayHeight();
    const int wndSizeY = (int)std::fmaf((float)displayHeight, m_logWndHeightPct, 0.5f);
    ImGui::SetNextWindowSize(ImVec2((float)m_headerWndWidth, (float)wndSizeY), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(0, (float)m_headerWndHeight), ImGuiCond_Always);

    ImGui::Begin("Logs", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);

    m_logWndHeightPct = ImGui::GetWindowHeight() / (float)displayHeight;

    auto& frameLogs = App::GetLogs().View();
    ImGui::Text("#Items: %d\t", (int)frameLogs.size());
    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_TRASH_CAN "  Clear"))
        frameLogs.clear();

    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_XMARK "  Close"))
        m_manuallyCloseLogsTab = true;

    ImGui::Separator();

    ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiChildFlags_AlwaysUseWindowPadding);

    // TODO consider using ImGuiListClipper
    for (auto& msg : frameLogs)
    {
        ImVec4 color = msg.Type == App::LogMessage::INFO ? ImVec4(0.3f, 0.4f, 0.5f, 1.0f) :
            ImVec4(0.4f, 0.2f, 0.2f, 1.0f);
        ImGui::TextColored(color, msg.Msg);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    ImGui::End();
}

void GuiPass::RenderMainHeader()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Tab, style.Colors[ImGuiCol_WindowBg]);
    ImGui::PushStyleColor(ImGuiCol_TabActive, 
        ImVec4(0.0295568369f, 0.0295568369f, 0.0295568369f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, 
        ImVec4(0.05098039215f, 0.05490196078f, 0.05490196078f, 1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(15, style.ItemInnerSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.WindowPadding.y));

    const int displayHeight = App::GetRenderer().GetDisplayHeight();
    const int wndHeight = (int)std::fmaf(m_headerWndHeightPct, (float)displayHeight, 0.5f);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)m_headerWndWidth, (float)wndHeight), ImGuiCond_Always);

    ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | 
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize);

    m_headerWndHeight = (int)ImGui::GetWindowHeight();

    ImGui::Text("        ");
    ImGui::SameLine();
    ImGui::BeginTabBar("Header", ImGuiTabBarFlags_None);

    auto flags = ImGuiTabItemFlags_None;
    if (m_manuallyCloseLogsTab)
    {
        flags = ImGuiTabItemFlags_SetSelected;
        m_manuallyCloseLogsTab = false;
    }
    const bool showMainWnd = ImGui::BeginTabItem(ICON_FA_DISPLAY "        Main        ", 
        nullptr, flags);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone))
    {
        ImGui::PopStyleVar();
        ImGui::SetTooltip("Scene View");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.WindowPadding.y));
    }

    if (showMainWnd)
        ImGui::EndTabItem();

    const bool renderGraphTab = ImGui::BeginTabItem(ICON_FA_SHARE_NODES "        Render Graph        ");

    if (ImGui::IsItemHovered())
    {
        ImGui::PopStyleVar();
        ImGui::SetTooltip("Render Graph Visualization (Use RMB for panning).");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.WindowPadding.y));
    }

    if (renderGraphTab)
    {
        const float headerWndHeight = ImGui::GetWindowHeight();

        ImGui::SetNextWindowSize(ImVec2((float)m_headerWndWidth, displayHeight - headerWndHeight),
            ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(0, headerWndHeight), ImGuiCond_Always);

        ImGui::Begin(" ", nullptr, ImGuiWindowFlags_NoMove);
        App::GetScene().DebugDrawRenderGraph();
        ImGui::End();

        ImGui::EndTabItem();
    }

    flags = ImGuiTabItemFlags_None;

    // Open the logs tabs whene there are new warnings
    if(!m_logsTabOpen)
    {
        auto& logs = App::GetLogs().View();
        const int numLogs = (int)logs.size();

        if (numLogs != m_prevNumLogs)
        {
            for (int i = m_prevNumLogs; i < numLogs; i++)
            {
                if (logs[i].Type == App::LogMessage::WARNING)
                {
                    flags = ImGuiTabItemFlags_SetSelected;
                    break;
                }
            }
        }

        m_prevNumLogs = numLogs;
    }

    m_logsTabOpen = ImGui::BeginTabItem(ICON_FA_TERMINAL "        Logs        ",
        nullptr, flags);
    if(m_logsTabOpen)
    {
        RenderLogWindow();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    ImGui::End();
}

void GuiPass::RenderGizmo(Span<uint64_t> pickedIDs, const TriangleMesh& mesh, const float4x4a& W)
{
    if (!ImGuizmo::IsUsingAny())
    {
        const auto& camera = App::GetCamera();
        auto& frustum = camera.GetCameraFrustumViewSpace();
        auto viewInv = camera.GetViewInv();

        // Transform view frustum from view space into world space
        v_float4x4 vViewInv = load4x4(const_cast<float4x4a&>(viewInv));
        v_ViewFrustum vFrustum(const_cast<ViewFrustum&>(frustum));
        vFrustum = transform(vViewInv, vFrustum);

        v_float4x4 vW = load4x4(W);
        v_AABB vBox(mesh.m_AABB);
        vBox = transform(vW, vBox);

        // Avoid drawing the gizmo if picked instance is outside the frustum
        if (instersectFrustumVsAABB(vFrustum, vBox) == COLLISION_TYPE::DISJOINT)
            return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

    float3 dt;
    float4x4a dr;
    float3 ds;
    float4x4a W_new = W;
    bool modified = ImGuizmo::Manipulate(static_cast<ImGuizmo::OPERATION>(m_currGizmoOperation), 
        ImGuizmo::WORLD, W_new, dt, dr, ds, nullptr);

    if (modified)
    {
        for(auto ID : pickedIDs)
            App::GetScene().TransformInstance(ID, dt, float3x3(dr), ds);
    }
}

void GuiPass::InfoTab()
{
    const float pad = 128.0f * App::GetDPIScaling();

    auto& renderer = App::GetRenderer();
    ImGui::Text(" - Device:");
    ImGui::SameLine(pad);
    ImGui::Text("%s", renderer.GetDeviceDescription());
    ImGui::Text(" - Render Resolution:");
    ImGui::SameLine(pad);
    ImGui::Text("%d x %d", renderer.GetRenderWidth(), 
        renderer.GetRenderHeight());
    ImGui::Text(" - Display Resolution:");
    ImGui::SameLine(pad);
    ImGui::Text("%d x %d (%u DPI)", renderer.GetDisplayWidth(), 
        renderer.GetDisplayHeight(), App::GetDPI());
}

void GuiPass::CameraTab()
{
    const Camera& camera = App::GetCamera();
    float3 camPos = camera.GetPos();
    float3 viewBasisX = camera.GetBasisX();
    float3 viewBasisY = camera.GetBasisY();
    float3 viewBasisZ = camera.GetBasisZ();

    const float pad = 220.0f * App::GetDPIScaling();
    ImGui::Text(" - Camera Position: (%.3f, %.3f, %.3f)", camPos.x, camPos.y, camPos.z);
    ImGui::SameLine(pad);
    if (ImGui::Button(ICON_FA_COPY " Copy##0"))
    {
        StackStr(buffer, n, "%.4f, %.4f, %.4f", camPos.x, camPos.y, camPos.z);
        App::CopyToClipboard(buffer);
    }
    ImGui::SetItemTooltip("Copy vector to clipboard");

    ImGui::Text(" - View Basis X: (%.3f, %.3f, %.3f)", viewBasisX.x, viewBasisX.y, viewBasisX.z);
    ImGui::Text(" - View Basis Y: (%.3f, %.3f, %.3f)", viewBasisY.x, viewBasisY.y, viewBasisY.z);
    ImGui::Text(" - View Basis Z: (%.3f, %.3f, %.3f)", viewBasisZ.x, viewBasisZ.y, viewBasisZ.z);
    ImGui::SameLine(pad);
    if (ImGui::Button(ICON_FA_COPY " Copy##1"))
    {
        StackStr(buffer, n, "%.4f, %.4f, %.4f", viewBasisZ.x, viewBasisZ.y, viewBasisZ.z);
        App::CopyToClipboard(buffer);
    }
    ImGui::SetItemTooltip("Copy vector to clipboard");

    ImGui::Text(" - Aspect Ratio: %f", camera.GetAspectRatio());

    constexpr int plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_Equal;

    if (ImPlot::BeginPlot("Camera Coordinate System", ImVec2(250.0f, 250.0f), plotFlags))
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

    ImGui::SeparatorText("Parameters");

    {
        auto params = App::GetParams();

        // Partition by "Scene"
        auto firstNonScene = std::partition(params.m_span.begin(), params.m_span.end(), 
            [](const ParamVariant& p)
            {
                return strcmp(p.GetGroup(), ICON_FA_LANDMARK " Scene") == 0;
            });
        // Partition by "Camera"
        auto firstNonCamera = std::partition(params.m_span.begin(), firstNonScene, 
            [](const ParamVariant& p)
            {
                return strcmp(p.GetSubGroup(), "Camera") == 0;
            });
        const int numCameraParams = (int)(firstNonCamera - params.m_span.data());
        if (numCameraParams == 0)
            return;

        std::sort(params.m_span.begin(), firstNonCamera,
            [](const ParamVariant& p1, const ParamVariant& p2)
            {
                return strcmp(p1.GetSubSubGroup(), p2.GetSubSubGroup()) < 0;
            });

        char curr[ParamVariant::MAX_SUBSUBGROUP_LEN];
        size_t len = strlen(params.m_span[0].GetSubSubGroup());
        memcpy(curr, params.m_span[0].GetSubSubGroup(), len);
        curr[len] = '\0';
        int beg = 0;
        int i = 0;

        for (i = 0; i < numCameraParams; i++)
        {
            if (strcmp(params.m_span[i].GetSubSubGroup(), curr) != 0)
            {
                if (ImGui::TreeNodeEx(curr))
                {
                    AddParamRange(params.m_span, beg, (int)(i - beg));
                    ImGui::TreePop();
                }

                len = strlen(params.m_span[i].GetSubSubGroup());
                memcpy(curr, params.m_span[i].GetSubSubGroup(), len);
                curr[len] = '\0';
                beg = i;
            }
        }

        if (ImGui::TreeNodeEx(curr))
        {
            AddParamRange(params.m_span, beg, i - beg);
            ImGui::TreePop();
        }
    }
}

void GuiPass::ParameterTab()
{
    auto params = App::GetParams();
    char currGroup[ParamVariant::MAX_GROUP_LEN];

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);

    // Sort by group
    std::sort(params.m_span.begin(), params.m_span.end(), 
        [](const ParamVariant& p1, const ParamVariant& p2)
        {
            return strcmp(p1.GetGroup(), p2.GetGroup()) < 0;
        });

    for (int currGroupIdx = 0; currGroupIdx < (int)params.m_span.size();)
    {
        ParamVariant& currParam_g = params.m_span[currGroupIdx];
        const size_t groupLen = strlen(currParam_g.GetGroup());
        memcpy(currGroup, currParam_g.GetGroup(), groupLen);
        currGroup[groupLen] = '\0';

        // Find the range of parameters for this group
        int nextGroupIdx = currGroupIdx;
        while (nextGroupIdx < params.m_span.size() && 
            (strcmp(params.m_span[nextGroupIdx].GetGroup(), currGroup) == 0))
            nextGroupIdx++;

        if (ImGui::CollapsingHeader(currParam_g.GetGroup(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            char currSubGroup[ParamVariant::MAX_SUBGROUP_LEN];

            // Sort by subgroup among current group
            std::sort(params.m_span.begin() + currGroupIdx, params.m_span.begin() + nextGroupIdx,
                [](const ParamVariant& p1, const ParamVariant& p2)
                {
                    return strcmp(p1.GetSubGroup(), p2.GetSubGroup()) < 0;
                });

            // Add the parameters in this subgroup
            for (int currSubgroupIdx = currGroupIdx; currSubgroupIdx < nextGroupIdx;)
            {
                ParamVariant& currParam_s = params.m_span[currSubgroupIdx];
                const char* subGroupName = currParam_s.GetSubGroup();
                const size_t subGroupLen = strlen(subGroupName);
                memcpy(currSubGroup, subGroupName, subGroupLen);
                currSubGroup[subGroupLen] = '\0';

                int nextSubgroupIdx = currSubgroupIdx;
                while (nextSubgroupIdx < params.m_span.size() &&
                    (strcmp(params.m_span[nextSubgroupIdx].GetSubGroup(), currSubGroup) == 0))
                    nextSubgroupIdx++;

                if (strcmp(subGroupName, "Camera") == 0)
                {
                    currSubgroupIdx = nextSubgroupIdx;
                    continue;
                }

                if (ImGui::TreeNode(currParam_s.GetSubGroup()))
                {
                    char currSubsubGroup[ParamVariant::MAX_SUBSUBGROUP_LEN];

                    // If there are no subsubgroups, show everything in just one subgroup
                    // instead of a subgroup with one empty subsubgroup
                    bool hasSubsubgroups = false;
                    for (int i = currSubgroupIdx; i < nextSubgroupIdx; i++)
                    {
                        if (params.m_span[i].GetSubSubGroup()[0] != '\0')
                        {
                            hasSubsubgroups = true;
                            break;
                        }
                    }

                    // Sort by subsubgroup among current subgroup
                    std::sort(params.m_span.begin() + currSubgroupIdx, params.m_span.begin() + nextSubgroupIdx,
                        [](const ParamVariant& p1, const ParamVariant& p2)
                        {
                            return strcmp(p1.GetSubSubGroup(), p2.GetSubSubGroup()) < 0;
                        });

                    if (hasSubsubgroups)
                    {
                        for (int currSubsubgroupIdx = currSubgroupIdx; currSubsubgroupIdx < nextSubgroupIdx;)
                        {
                            ParamVariant& currParam_ss = params.m_span[currSubsubgroupIdx];
                            const size_t subSubgroupLen = strlen(currParam_ss.GetSubSubGroup());
                            memcpy(currSubsubGroup, currParam_ss.GetSubSubGroup(), subSubgroupLen);
                            currSubsubGroup[subSubgroupLen] = '\0';

                            int nextSubsubgroupIdx = currSubsubgroupIdx;
                            while (nextSubsubgroupIdx < params.m_span.size() &&
                                (strcmp(params.m_span[nextSubsubgroupIdx].GetSubSubGroup(), currSubsubGroup) == 0))
                                nextSubsubgroupIdx++;

                            if (subSubgroupLen > 0)
                            {
                                ImGui::SeparatorText(currParam_ss.GetSubSubGroup());
                                AddParamRange(params.m_span, currSubsubgroupIdx, nextSubsubgroupIdx - currSubsubgroupIdx);
                            }
                            else
                                AddParamRange(params.m_span, currSubsubgroupIdx, nextSubsubgroupIdx - currSubsubgroupIdx);

                            currSubsubgroupIdx = nextSubsubgroupIdx;
                        }
                    }
                    else
                        AddParamRange(params.m_span, currSubgroupIdx, nextSubgroupIdx - currSubgroupIdx);

                    ImGui::TreePop();
                }

                currSubgroupIdx = nextSubgroupIdx;
            }

            ImGui::Text("");
        }

        currGroupIdx = nextGroupIdx;
    }

    ImGui::PopItemWidth();
}

void GuiPass::GpuTimingsTab()
{
    if (App::GetTimer().GetTotalFrameCount() % 4 == 0)
    {
        auto timings = App::GetRenderer().GetGpuTimer().GetFrameTimings();
        m_cachedTimings.clear();
        m_cachedTimings.append_range(timings.begin(), timings.end());

        if (m_cachedTimings.size() > 0)
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
    auto handlers = App::GetShaderReloadHandlers();

    if (!handlers.m_span.empty())
    {
        std::sort(handlers.m_span.begin(), handlers.m_span.end(), 
            [](const App::ShaderReloadHandler& lhs, const App::ShaderReloadHandler& rhs)
            {
                return strcmp(lhs.Name, rhs.Name) < 0;
            });
    }

    ImGui::Text("Select a shader to reload:");

    // TODO m_currShader becomes invalid when there's been a change in reloadHandlers 
    if (ImGui::BeginCombo("Shader", m_currShader >= 0 ? handlers.m_span[m_currShader].Name : "None", 0))
    {
        int i = 0;

        for (auto& handler : handlers.m_span)
        {
            bool selected = (m_currShader == i);
            if (ImGui::Selectable(handler.Name, selected))
                m_currShader = i;

            if (selected)
                ImGui::SetItemDefaultFocus();

            i++;
        }

        ImGui::EndCombo();
    }

    if (m_currShader == -1)
        ImGui::BeginDisabled();

    if (ImGui::Button("Reload"))
        handlers.m_span[m_currShader].Dlg();

    if (m_currShader == -1)
        ImGui::EndDisabled();
}

void GuiPass::PickedWorldTransform(uint64 pickedID, const TriangleMesh& mesh, const float4x4a& W)
{
    // Instance info
    if (ImGui::TreeNodeEx("Info", ImGuiTreeNodeFlags_NoTreePushOnOpen))
    {
        const float pad = 96.0f * App::GetDPIScaling();

        ImGui::Text(" - ID:");
        ImGui::SameLine(pad);
        ImGui::Text("%llu", pickedID);
        ImGui::Text(" - #Vertices:");
        ImGui::SameLine(pad);
        ImGui::Text("%u", mesh.m_numVertices);
        ImGui::Text(" - #Triangles:");
        ImGui::SameLine(pad);
        ImGui::Text("%u", mesh.m_numIndices / 3);
        ImGui::Text(" - Material ID:");
        ImGui::SameLine(pad);
        ImGui::Text("%u", mesh.m_materialID);
        ImGui::Text("");
    }

    bool modified = false;
    auto& scene = App::GetScene();
    AffineTransformation prevTr = AffineTransformation::GetIdentity();
    AffineTransformation newTr = AffineTransformation::GetIdentity();

    if (ImGui::TreeNodeEx("Transformation", ImGuiTreeNodeFlags_NoTreePushOnOpen))
    {
        const bool isLocal = m_transform == TRANSFORMATION::LOCAL;

        if (isLocal)
        {
            prevTr = scene.GetLocalTransform(pickedID);
            newTr = prevTr;
        }
        else
        {
            v_float4x4 vW = load4x4(W);

            float4a s;
            float4a r;
            float4a t;
            decomposeSRT(vW, s, r, t);

            newTr.Translation = t.xyz();
            newTr.Rotation = r;
            newTr.Scale = s.xyz();
        }

        auto axisOrQuatXyz = newTr.Rotation.xyz();
        float angle_r = newTr.Rotation.w;
        float angle_d = newTr.Rotation.w;
        if (m_rotationMode == ROTATION_MODE::AXIS_ANGLE)
        {
            quaternionToAxisAngle(newTr.Rotation, axisOrQuatXyz, angle_r);
            angle_d = RadiansToDegrees(angle_r);
        }

        // Transformation mode
        {
            const float pad = 48.0f * App::GetDPIScaling();
            const char* modes[] = { "Local", "World" };
            ImGui::Text("");
            ImGui::SameLine(pad);
            ImGui::Text("Mode");
            ImGui::SameLine();
            ImGui::Combo("##20", (int*)&m_transform, modes, ZetaArrayLen(modes));
        }

        if (!isLocal)
            ImGui::BeginDisabled();

        // Translation
        {
            ImGui::Text("Translation X");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##0", &newTr.Translation.x, -50.0f, 50.0f, "%.2f"))
                modified = true;

            const float pad = 69.6f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad);
            ImGui::Text("Y");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##1", &newTr.Translation.y, -15.0f, 15.0f, "%.2f"))
                modified = true;

            ImGui::Text("");
            ImGui::SameLine(pad);
            ImGui::Text("Z");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##2", &newTr.Translation.z, -50.0f, 50.0f, "%.2f"))
                modified = true;
        }

        // Rotation
        {
            // When angle = 0 or 2 PI, setting axis results in a zero quaternion and would
            // have the effect of UI change not applying. As a workaround use a small
            // angle =~ zero.
            constexpr float MIN_ANGLE = 1e-5f;
            constexpr float MAX_ANGLE = TWO_PI - 1e-5f;

            auto getQuat = [](float3& n, float theta)
                {
                    n.normalize();
                    theta = Min(theta, MAX_ANGLE);
                    theta = Max(theta, MIN_ANGLE);
                    return float4(n * sinf(0.5f * theta), cosf(0.5f * theta));
                };

            const float pad_x = 21.6f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_x);
            ImGui::Text("Rotation X");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##4", &axisOrQuatXyz.x, -1.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                if (m_rotationMode == ROTATION_MODE::AXIS_ANGLE)
                    newTr.Rotation = getQuat(axisOrQuatXyz, angle_r);
                else
                {
                    newTr.Rotation = float4(axisOrQuatXyz, prevTr.Rotation.w);
                    newTr.Rotation.normalize();
                }

                modified = true;
            }

            const float pad_y = 69.6f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_y);
            ImGui::Text("Y");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##5", &axisOrQuatXyz.y, -1.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                if (m_rotationMode == ROTATION_MODE::AXIS_ANGLE)
                    newTr.Rotation = getQuat(axisOrQuatXyz, angle_r);
                else
                {
                    newTr.Rotation = float4(axisOrQuatXyz, prevTr.Rotation.w);
                    newTr.Rotation.normalize();
                }

                modified = true;
            }

            const float pad_z = 68.8f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_z);
            ImGui::Text("Z");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##6", &axisOrQuatXyz.z, -1.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                if (m_rotationMode == ROTATION_MODE::AXIS_ANGLE)
                    newTr.Rotation = newTr.Rotation = getQuat(axisOrQuatXyz, angle_r);
                else
                {
                    newTr.Rotation = float4(axisOrQuatXyz, prevTr.Rotation.w);
                    newTr.Rotation.normalize();
                }

                modified = true;
            }

            const float pad_w = 65.6f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_w);
            ImGui::Text("W");
            ImGui::SameLine();
            const float range_min = m_rotationMode == ROTATION_MODE::AXIS_ANGLE ? 0.0f : -1.0f;
            const float range_max = m_rotationMode == ROTATION_MODE::AXIS_ANGLE ? 360.0f : 1.0f;
            float* w = m_rotationMode == ROTATION_MODE::AXIS_ANGLE ? &angle_d : &newTr.Rotation.w;
            if (ImGui::SliderFloat("##7", w, range_min, range_max, "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                if (m_rotationMode == ROTATION_MODE::AXIS_ANGLE)
                {
                    angle_r = DegreesToRadians(angle_d);
                    angle_r = Min(angle_r, MAX_ANGLE);
                    angle_r = Max(angle_r, MIN_ANGLE);
                    newTr.Rotation = float4(axisOrQuatXyz * sinf(0.5f * angle_r), cosf(0.5f * angle_r));
                }
                else
                    newTr.Rotation.normalize();

                modified = true;
            }

            const float pad_m = 48.0f * App::GetDPIScaling();
            const char* modes[] = { "Axis Angle", "Quaternion (XYZW)" };
            ImGui::Text("");
            ImGui::SameLine(pad_m);
            ImGui::Text("Mode");
            ImGui::SameLine();
            ImGui::Combo("##10", (int*)&m_rotationMode, modes, ZetaArrayLen(modes));
        }

        // Scale
        {
            // To avoid scale = 0
            constexpr float MIN_SCALE_RATIO = 1e-3f;

            const float pad_x = 37.6f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_x);
            ImGui::Text("Scale X");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##11", &newTr.Scale.x, MIN_SCALE_RATIO, 20.0f, "%.3f"))
            {
                // Clamp from below but not above
                newTr.Scale.x = Max(MIN_SCALE_RATIO, newTr.Scale.x);
                modified = true;
            }

            const float pad_y = 69.6f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_y);
            ImGui::Text("Y");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##12", &newTr.Scale.y, MIN_SCALE_RATIO, 20.0f, "%.3f"))
            {
                newTr.Scale.y = Max(MIN_SCALE_RATIO, newTr.Scale.y);
                modified = true;
            }

            const float pad_z = 68.8f * App::GetDPIScaling();
            ImGui::Text("");
            ImGui::SameLine(pad_z);
            ImGui::Text("Z");
            ImGui::SameLine();
            if (ImGui::SliderFloat("##13", &newTr.Scale.z, MIN_SCALE_RATIO, 20.0f, "%.3f"))
            {
                newTr.Scale.z = Max(MIN_SCALE_RATIO, newTr.Scale.z);
                modified = true;
            }
        }

        if (!isLocal)
            ImGui::EndDisabled();
    }

    if (modified)
    {
        v_float4x4 vR_new = rotationMatFromQuat(loadFloat4(newTr.Rotation));
        v_float4x4 vR_prev = rotationMatFromQuat(loadFloat4(prevTr.Rotation));
        // Inverse of existing rotation
        v_float4x4 vR_prev_inv = transpose(vR_prev);
        vR_new = mul(vR_prev_inv, vR_new);
        float3x3 R = float3x3(store(vR_new));

        scene.TransformInstance(pickedID, newTr.Translation - prevTr.Translation,
            R, 
            newTr.Scale / prevTr.Scale);
    }
}

void GuiPass::PickedMaterial(uint64 pickedID)
{
    const bool pickChangedFromLastTime = m_lastPickedID != pickedID;
    m_lastPickedID = pickedID;

    auto& scene = App::GetScene();
    const auto meshID = scene.GetInstanceMeshID(pickedID);
    const auto& mesh = *scene.GetMesh(meshID).value();
    Material mat = *scene.GetMaterial(mesh.m_materialID).value();
    bool modified = false;
    constexpr ImVec4 texturedCol = ImVec4(0.9587256, 0.76055556, 0.704035435, 1);

    if (ImGui::TreeNode("Base"))
    {
        float3 color = mat.GetBaseColorFactor();
        const bool baseColorTextured = mat.GetBaseColorTex() != Material::INVALID_ID;
        const bool mrTextured = mat.GetMetallicRoughnessTex() != Material::INVALID_ID;
        bool metallic = mat.Metallic();

        if (baseColorTextured)
            ImGui::PushStyleColor(ImGuiCol_Text, texturedCol);

        if (ImGui::ColorEdit3("Color", reinterpret_cast<float*>(&color), ImGuiColorEditFlags_Float))
        {
            mat.SetBaseColorFactor(color);
            modified = true;
        }

        if (baseColorTextured)
            ImGui::PopStyleColor();

        if (mrTextured)
            ImGui::PushStyleColor(ImGuiCol_Text, texturedCol);

        const bool disabled = mat.Transmissive();
        if (disabled)
            ImGui::BeginDisabled();

        if (ImGui::Checkbox("Metallic", &metallic))
        {
            mat.SetMetallic(metallic);
            modified = true;
        }

        if (disabled)
            ImGui::EndDisabled();

        if (mrTextured)
            ImGui::PopStyleColor();

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Specular"))
    {
        float roughness = mat.GetSpecularRoughness();
        float ior = mat.GetSpecularIOR();
        const bool mrTextured = mat.GetMetallicRoughnessTex() != Material::INVALID_ID;

        if (mrTextured)
            ImGui::PushStyleColor(ImGuiCol_Text, texturedCol);

        if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.2f"))
        {
            mat.SetSpecularRoughness(roughness);
            modified = true;
        }

        if (mrTextured)
            ImGui::PopStyleColor();

        if (ImGui::SliderFloat("IOR", &ior, MIN_IOR, MAX_IOR, "%.2f"))
        {
            mat.SetSpecularIOR(ior);
            modified = true;
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Transmission"))
    {
        bool transmissive = mat.Transmissive();
        const bool baseColorTextured = mat.GetBaseColorTex() != Material::INVALID_ID;
        float3 color = mat.GetBaseColorFactor();
        float depth = HalfToFloat(mat.GetTransmissionDepth().x);
        const bool disabled = mat.Metallic() || mat.ThinWalled();

        if (disabled)
            ImGui::BeginDisabled();

        if (ImGui::Checkbox("Transmissive", &transmissive))
        {
            mat.SetTransmission(transmissive ? 1.0f : 0.0f);
            modified = true;
        }

        if (baseColorTextured)
            ImGui::PushStyleColor(ImGuiCol_Text, texturedCol);

        if (ImGui::ColorEdit3("Color", reinterpret_cast<float*>(&color), ImGuiColorEditFlags_Float))
        {
            mat.SetBaseColorFactor(color);
            modified = true;
        }

        if (baseColorTextured)
            ImGui::PopStyleColor();

        if (ImGui::SliderFloat("Depth", &depth, 0.0f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic))
        {
            mat.SetTransmissionDepth(depth);
            modified = true;
        }

        if (disabled)
            ImGui::EndDisabled();

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Subsurface"))
    {
        float subsurface = mat.GetSubsurface();
        const bool disabled = mat.Metallic() || mat.Transmissive() || !mat.ThinWalled();

        if (disabled)
            ImGui::BeginDisabled();

        if (ImGui::SliderFloat("Weight", &subsurface, 0.0f, 1.0f, "%.2f"))
        {
            mat.SetSubsurface(subsurface);
            modified = true;
        }

        if (disabled)
            ImGui::EndDisabled();

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Coat"))
    {
        float weight = mat.GetCoatWeight();
        float3 color = mat.GetCoatColor();
        float roughness = mat.GetCoatRoughness();
        float ior = mat.GetCoatIOR();

        if (ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f, "%.2f"))
        {
            mat.SetCoatWeight(weight);
            modified = true;
        }

        if (ImGui::ColorEdit3("Color", reinterpret_cast<float*>(&color), ImGuiColorEditFlags_Float))
        {
            mat.SetCoatColor(color);
            modified = true;
        }

        if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.2f"))
        {
            mat.SetCoatRoughness(roughness);
            modified = true;
        }

        if (ImGui::SliderFloat("IOR", &ior, MIN_IOR, MAX_IOR, "%.2f"))
        {
            mat.SetCoatIOR(ior);
            modified = true;
        }

        ImGui::TreePop();
    }

    float3 emissiveFactor = mat.GetEmissiveFactor();
    float emissiveStrength = HalfToFloat(mat.GetEmissiveStrength().x);
    bool colorEditFinished = false;
    bool strEditFinished = false;

    if (ImGui::TreeNode("Emission"))
    {
        const bool textured = mat.GetEmissiveTex() != Material::INVALID_ID;

        if (!mat.Emissive())
            ImGui::BeginDisabled();

        const float3 oldColor = emissiveFactor;
        const float oldStr = emissiveStrength;

        if (textured)
            ImGui::PushStyleColor(ImGuiCol_Text, texturedCol);

        const char* modes[] = { "(Linear) RGB", "Temperature" };
        m_emissiveColorMode = pickChangedFromLastTime ? EMISSIVE_COLOR_MODE::RGB : m_emissiveColorMode;
        bool colorModeChanged = ImGui::Combo("Color Mode", (int*)&m_emissiveColorMode, modes, ZetaArrayLen(modes));
        bool switchedToTemperature = false;

        if (m_emissiveColorMode == EMISSIVE_COLOR_MODE::RGB)
        {
            if (ImGui::ColorEdit3("Color", reinterpret_cast<float*>(&emissiveFactor), ImGuiColorEditFlags_Float))
            {
                float3 diff = oldColor - emissiveFactor;

                // Avoid spamming update when difference is close to zero
                if (diff.dot(diff) > 1e-5)
                {
                    mat.SetEmissiveFactor(emissiveFactor);
                    modified = true;
                    m_pendingEmissiveUpdate = true;
                }
            }
        }
        else
        {
            switchedToTemperature = colorModeChanged;
            m_currColorTemperature = switchedToTemperature ? DEFAULT_COLOR_TEMPERATURE : 
                m_currColorTemperature;
            if (switchedToTemperature || ImGui::SliderFloat("Temperature", &m_currColorTemperature, 1000, 40000, "%.2f"))
            {
                emissiveFactor = sRGBToLinear(ColorTemperatureTosRGB(m_currColorTemperature));
                float3 diff = oldColor - emissiveFactor;

                // Avoid spamming update when difference is close to zero
                if (diff.dot(diff) > 1e-5)
                {
                    mat.SetEmissiveFactor(emissiveFactor);
                    modified = true;
                    m_pendingEmissiveUpdate = true;
                }
            }
        }

        colorEditFinished = switchedToTemperature || ImGui::IsItemDeactivatedAfterEdit();

        if (textured)
            ImGui::PopStyleColor();

        if (ImGui::SliderFloat("Strength", &emissiveStrength, 0, 50, "%.3f"))
        {
            if (fabsf(oldStr - emissiveStrength) > 1e-2)
            {
                mat.SetEmissiveStrength(emissiveStrength);
                modified = true;
                m_pendingEmissiveUpdate = true;
            }
        }

        strEditFinished = ImGui::IsItemDeactivatedAfterEdit();

        if (!mat.Emissive())
            ImGui::EndDisabled();

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Geometry"))
    {
        bool doubleSided = mat.DoubleSided();
        bool thinWalled = mat.ThinWalled();

        if (ImGui::Checkbox("Double Sided", &doubleSided))
        {
            mat.SetDoubleSided(doubleSided);
            modified = true;
        }

        const bool disabled = mat.Transmissive() || !mat.DoubleSided();
        if (disabled)
            ImGui::BeginDisabled();

        if (ImGui::Checkbox("Thin Walled", &thinWalled))
        {
            mat.SetThinWalled(thinWalled);
            modified = true;
        }

        if (disabled)
            ImGui::EndDisabled();

        ImGui::TreePop();
    }

    if (modified)
        scene.UpdateMaterial(mesh.m_materialID, mat);

    // Defer update to when user has stopped editing
    if (m_pendingEmissiveUpdate && (colorEditFinished || strEditFinished))
    {
        scene.UpdateEmissiveMaterial(pickedID, emissiveFactor, emissiveStrength);
        m_pendingEmissiveUpdate = false;
    }
}
