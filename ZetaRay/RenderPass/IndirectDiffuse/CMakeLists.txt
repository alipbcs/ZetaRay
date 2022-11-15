set(RP_IND_DIFF_DIR ${RENDERPASS_DIR}/IndirectDiffuse)
set(RP_IND_DIFF_SRC
    ${RP_IND_DIFF_DIR}/ReSTIR_GI.cpp
    ${RP_IND_DIFF_DIR}/ReSTIR_GI.h
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Common.h
    ${RP_IND_DIFF_DIR}/Reservoir.hlsli
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_Validation.hlsl
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_TemporalPass.hlsl
    ${RP_IND_DIFF_DIR}/ReSTIR_GI_SpatialPass.hlsl)
set(RP_IND_DIFF_SRC ${RP_IND_DIFF_SRC} PARENT_SCOPE)
