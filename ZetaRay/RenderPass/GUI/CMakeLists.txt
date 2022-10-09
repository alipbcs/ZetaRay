set(RP_GUI_DIR ${RENDERPASS_DIR}/GUI)
set(RP_GUI_SRC
    ${RP_GUI_DIR}/GuiPass.cpp
    ${RP_GUI_DIR}/GuiPass.h
    ${RP_GUI_DIR}/ImGui.hlsl
    ${RP_GUI_DIR}/GuiPass_Common.h)
set(RP_GUI_SRC ${RP_GUI_SRC} PARENT_SCOPE)