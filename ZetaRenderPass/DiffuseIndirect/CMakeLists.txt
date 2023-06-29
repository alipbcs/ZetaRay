set(RP_DIFF_IND_DIR ${ZETA_RENDER_PASS_DIR}/DiffuseIndirect)
set(RP_DIFF_IND_SRC
    ${RP_DIFF_IND_DIR}/ReSTIR_GI_Diffuse.cpp
    ${RP_DIFF_IND_DIR}/ReSTIR_GI_Diffuse.h
    ${RP_DIFF_IND_DIR}/ReSTIR_GI_Diffuse_Common.h
    ${RP_DIFF_IND_DIR}/Reservoir_Diffuse.hlsli
    ${RP_DIFF_IND_DIR}/ReSTIR_GI_Diffuse_Validation.hlsl
    ${RP_DIFF_IND_DIR}/ReSTIR_GI_Diffuse_Temporal.hlsl
    ${RP_DIFF_IND_DIR}/ReSTIR_GI_Diffuse_Spatial.hlsl
    ${RP_DIFF_IND_DIR}/DiffuseDNSR_Temporal.hlsl
    ${RP_DIFF_IND_DIR}/DiffuseDNSR_SpatialFilter.hlsl)
set(RP_DIFF_IND_SRC ${RP_DIFF_IND_SRC} PARENT_SCOPE)
