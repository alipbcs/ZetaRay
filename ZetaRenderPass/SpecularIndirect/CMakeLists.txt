set(RP_SPEC_IND_DIR ${ZETA_RENDER_PASS_DIR}/SpecularIndirect)
set(RP_SPEC_IND_SRC
    ${RP_SPEC_IND_DIR}/ReSTIR_GI_Specular.cpp
    ${RP_SPEC_IND_DIR}/ReSTIR_GI_Specular.h
    ${RP_SPEC_IND_DIR}/ReSTIR_GI_Specular_Common.h
    ${RP_SPEC_IND_DIR}/Reservoir_Specular.hlsli
    ${RP_SPEC_IND_DIR}/ReSTIR_GI_Specular_Temporal.hlsl
    ${RP_SPEC_IND_DIR}/ReSTIR_GI_Specular_Spatial.hlsl
    ${RP_SPEC_IND_DIR}/SpecularDNSR_Temporal.hlsl)
set(RP_SPEC_IND_SRC ${RP_SPEC_IND_SRC} PARENT_SCOPE)
