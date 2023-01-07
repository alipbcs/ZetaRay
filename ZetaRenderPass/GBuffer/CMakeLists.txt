set(RP_GBUFFER_DIR ${ZETA_RENDER_PASS_DIR}/GBuffer)
set(RP_GBUFFER_SRC
    ${RP_GBUFFER_DIR}/GBufferPass.cpp
    ${RP_GBUFFER_DIR}/GBufferPass.h
    ${RP_GBUFFER_DIR}/GBuffer_Common.h
    ${RP_GBUFFER_DIR}/GBuffer.hlsl
    ${RP_GBUFFER_DIR}/BuildDrawIndArgs_NoCull.hlsl
    ${RP_GBUFFER_DIR}/BuildDrawIndArgs_OcclusionCull.hlsl
    ${RP_GBUFFER_DIR}/DepthPyramid.hlsl)
set(RP_GBUFFER_SRC ${RP_GBUFFER_SRC} PARENT_SCOPE)
