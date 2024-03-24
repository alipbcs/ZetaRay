set(RP_IND_LIGHTING_DIR ${ZETA_RENDER_PASS_DIR}/IndirectLighting)
set(RP_IND_LIGHTING_SRC
	${RP_IND_LIGHTING_DIR}/IndirectLighting.cpp
    ${RP_IND_LIGHTING_DIR}/IndirectLighting.h
    ${RP_IND_LIGHTING_DIR}/IndirectLighting_Common.h
    ${RP_IND_LIGHTING_DIR}/NEE.hlsli
    ${RP_IND_LIGHTING_DIR}/Params.hlsli
    ${RP_IND_LIGHTING_DIR}/PathTracing.hlsli
    ${RP_IND_LIGHTING_DIR}/RayQuery.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI_PairwiseMIS.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI_PathTracing.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI_Resampling.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI_Reservoir.hlsli
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI_LVG.hlsl
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI_NPS.hlsl
    ${RP_IND_LIGHTING_DIR}/ReSTIR_GI/ReSTIR_GI.hlsl
    ${RP_IND_LIGHTING_DIR}/IndirectDnsr_Temporal.hlsl
    ${RP_IND_LIGHTING_DIR}/IndirectDnsr_Spatial.hlsl)
set(RP_IND_LIGHTING_SRC ${RP_IND_LIGHTING_SRC} PARENT_SCOPE)