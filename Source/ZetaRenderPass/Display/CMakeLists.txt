set(RP_DISPLAY_DIR ${ZETA_RENDER_PASS_DIR}/Display)
set(RP_DISPLAY_SRC
    ${RP_DISPLAY_DIR}/Display.cpp
    ${RP_DISPLAY_DIR}/Display.h
    ${RP_DISPLAY_DIR}/Display.hlsl
    ${RP_DISPLAY_DIR}/Tonemap.hlsli
	${RP_DISPLAY_DIR}/DoF_CoC.hlsl
	${RP_DISPLAY_DIR}/DoF_Gather.hlsl
	${RP_DISPLAY_DIR}/DoF_GaussianFilter.hlsl
    ${RP_DISPLAY_DIR}/Display_Common.h)

set(RP_DISPLAY_SRC ${RP_DISPLAY_SRC} PARENT_SCOPE)