set(RP_DI_DIR ${ZETA_RENDER_PASS_DIR}/DirectIllumination)
set(RP_DI_SRC
    ${RP_DI_DIR}/ReSTIR_DI.cpp
    ${RP_DI_DIR}/ReSTIR_DI.h
    ${RP_DI_DIR}/ReSTIR_DI_Common.h
    ${RP_DI_DIR}/Reservoir_DI.hlsli
    ${RP_DI_DIR}/ReSTIR_DI_Temporal.hlsl
    ${RP_DI_DIR}/ReSTIR_DI_Spatial.hlsl
    ${RP_DI_DIR}/DirectDNSR_Temporal.hlsl
    ${RP_DI_DIR}/DirectDNSR_SpatialFilter.hlsl)
set(RP_DI_SRC ${RP_DI_SRC} PARENT_SCOPE)
