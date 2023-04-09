set(RP_IND_SPEC_DIR ${ZETA_RENDER_PASS_DIR}/IndirectSpecular)
set(RP_IND_SPEC_SRC
    ${RP_IND_SPEC_DIR}/ReSTIR_GI_Specular.cpp
    ${RP_IND_SPEC_DIR}/ReSTIR_GI_Specular.h
    ${RP_IND_SPEC_DIR}/ReSTIR_GI_Specular_Common.h
    ${RP_IND_SPEC_DIR}/Reservoir_Specular.hlsli
    ${RP_IND_SPEC_DIR}/EstimateCurvature.hlsl
    ${RP_IND_SPEC_DIR}/ReSTIR_GI_Specular_Temporal.hlsl
    ${RP_IND_SPEC_DIR}/ReSTIR_GI_Specular_Spatial.hlsl
    ${RP_IND_SPEC_DIR}/SpecularDNSR_Temporal.hlsl)
set(RP_IND_SPEC_SRC ${RP_IND_SPEC_SRC} PARENT_SCOPE)
