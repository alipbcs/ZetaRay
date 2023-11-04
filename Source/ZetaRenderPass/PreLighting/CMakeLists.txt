set(RP_PRE_LIGHTING_DIR ${ZETA_RENDER_PASS_DIR}/PreLighting)
set(RP_PRE_LIGHTING_SRC
	${RP_PRE_LIGHTING_DIR}/PreLighting.cpp
    ${RP_PRE_LIGHTING_DIR}/PreLighting.h
    ${RP_PRE_LIGHTING_DIR}/PreLighting_Common.h
    ${RP_PRE_LIGHTING_DIR}/EstimateTriLumen.hlsl
    ${RP_PRE_LIGHTING_DIR}/PresampleEmissives.hlsl
    ${RP_PRE_LIGHTING_DIR}/EstimateCurvature.hlsl)
set(RP_PRE_LIGHTING_SRC ${RP_PRE_LIGHTING_SRC} PARENT_SCOPE)