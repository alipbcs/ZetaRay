set(RP_IND_DIFF_DIR ${ZETA_RENDER_PASS_DIR}/IndirectDiffuse)
set(RP_IND_DIFF_SRC
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Diffuse.cpp
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Diffuse.h
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Diffuse_Common.h
    ${RP_IND_DIFF_DIR}/Reservoir_Diffuse.hlsli
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Diffuse_Validation.hlsl
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Diffuse_Temporal.hlsl
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Diffuse_Spatial.hlsl)
set(RP_IND_DIFF_SRC ${RP_IND_DIFF_SRC} PARENT_SCOPE)
