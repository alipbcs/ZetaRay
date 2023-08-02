set(RP_DL_DIR ${ZETA_RENDER_PASS_DIR}/DirectLighting)
set(RP_DL_SRC
	${RP_DL_DIR}/DirectLighting.cpp
    ${RP_DL_DIR}/DirectLighting.h
    ${RP_DL_DIR}/DirectLighting_Common.h
    ${RP_DL_DIR}/EstimateTriLumen.hlsl
    ${RP_DL_DIR}/ReSTIR_DI_SpatioTemporal.hlsl
    ${RP_DL_DIR}/ReSTIR_DI_SpatioTemporal_LP.hlsl
    ${RP_DL_DIR}/ReSTIR_DI_Reservoir.hlsli
    ${RP_DL_DIR}/ReSTIR_DI_Resampling.hlsli
    ${RP_DL_DIR}/ReSTIR_DI_Presample.hlsl
    ${RP_DL_DIR}/ReSTIR_DI_DNSR_Temporal.hlsl
    ${RP_DL_DIR}/ReSTIR_DI_DNSR_Spatial.hlsl
    ${RP_DL_DIR}/ReSTIR_DI.hlsli)
set(RP_DL_SRC ${RP_DL_SRC} PARENT_SCOPE)