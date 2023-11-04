set(RP_IND_LIGHTING_DIR ${ZETA_RENDER_PASS_DIR}/IndirectLighting)
set(RP_IND_LIGHTING_SRC
	${RP_IND_LIGHTING_DIR}/IndirectLighting.cpp
    ${RP_IND_LIGHTING_DIR}/IndirectLighting.h
    ${RP_IND_LIGHTING_DIR}/IndirectLighting_Common.h
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_Common.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI.hlsl
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_LVG.hlsl
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_NPS.hlsl
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_Trace.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_Reservoir.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_Resampling.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_NEE.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI_PairwiseMIS.hlsli
    ${RP_IND_LIGHTING_DIR}/IndirectDnsr_Temporal.hlsl
    ${RP_IND_LIGHTING_DIR}/IndirectDnsr_Spatial.hlsl)
set(RP_IND_LIGHTING_SRC ${RP_IND_LIGHTING_SRC} PARENT_SCOPE)