set(RP_GBUFFER_DIR ${RENDERPASS_DIR}/GBuffer)
set(RP_GBUFFER_SRC
    ${RP_GBUFFER_DIR}/GBuffer.cpp
    ${RP_GBUFFER_DIR}/GBuffer.h
    ${RP_GBUFFER_DIR}/GBuffer_Common.h
    ${RP_GBUFFER_DIR}/GBuffer.hlsl
    ${RP_GBUFFER_DIR}/OcclusionCulling.hlsl)
set(RP_GBUFFER_SRC ${RP_GBUFFER_SRC} PARENT_SCOPE)
