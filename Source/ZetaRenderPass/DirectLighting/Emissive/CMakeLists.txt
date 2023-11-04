set(RP_EMISSIVE_DI_DIR ${ZETA_RENDER_PASS_DIR}/DirectLighting/Emissive)
set(RP_EMISSIVE_DI_SRC
	${RP_EMISSIVE_DI_DIR}/DirectLighting.cpp
    ${RP_EMISSIVE_DI_DIR}/DirectLighting.h
    ${RP_EMISSIVE_DI_DIR}/DirectLighting_Common.h
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_SpatioTemporal.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_SpatioTemporal_LP.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_Reservoir.hlsli
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_Resampling.hlsli
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_DNSR_Temporal.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_DNSR_Spatial.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI.hlsli)
set(RP_EMISSIVE_DI_SRC ${RP_EMISSIVE_DI_SRC} PARENT_SCOPE)