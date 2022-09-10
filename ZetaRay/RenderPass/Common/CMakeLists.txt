set(ZETA_RP_COMMON_DIR ${ZETA_RENDERPASS_DIR}/Common)
set(ZETA_RP_COMMON_SRC
    ${ZETA_RP_COMMON_DIR}/BRDF.hlsli
    ${ZETA_RP_COMMON_DIR}/Common.hlsli
    ${ZETA_RP_COMMON_DIR}/FrameConstants.h
    ${ZETA_RP_COMMON_DIR}/GBuffers.hlsli
    ${ZETA_RP_COMMON_DIR}/HLSLCompat.h
    ${ZETA_RP_COMMON_DIR}/Material.h
    ${ZETA_RP_COMMON_DIR}/RT.hlsli
    ${ZETA_RP_COMMON_DIR}/RtCommon.h
    ${ZETA_RP_COMMON_DIR}/Sampling.hlsli
    ${ZETA_RP_COMMON_DIR}/SH.hlsli
    ${ZETA_RP_COMMON_DIR}/StaticTextureSamplers.hlsli
    ${ZETA_RP_COMMON_DIR}/VolumetricLighting.hlsli)
set(ZETA_RP_COMMON_SRC ${ZETA_RP_COMMON_SRC} PARENT_SCOPE)