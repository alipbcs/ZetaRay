set(RP_GBUFFER_RT_DIR ${ZETA_RENDER_PASS_DIR}/GBuffer)
set(RP_GBUFFER_RT_SRC
    ${RP_GBUFFER_RT_DIR}/GBufferRT.cpp
    ${RP_GBUFFER_RT_DIR}/GBufferRT.h
    ${RP_GBUFFER_RT_DIR}/GBufferRT_Common.h
    ${RP_GBUFFER_RT_DIR}/GBufferRT_Inline.hlsl
    ${RP_GBUFFER_RT_DIR}/GBufferRT.hlsli
    ${RP_GBUFFER_RT_DIR}/GenerateDepthBuffer.h
    ${RP_GBUFFER_RT_DIR}/GenerateDepthBuffer.cpp
    ${RP_GBUFFER_RT_DIR}/GenerateDepthBuffer.hlsl)
set(RP_GBUFFER_RT_SRC ${RP_GBUFFER_RT_SRC} PARENT_SCOPE)
