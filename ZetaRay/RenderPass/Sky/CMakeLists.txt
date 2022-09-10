set(ZETA_RP_SKY_DIR "${ZETA_RENDERPASS_DIR}/Sky")
set(ZETA_RP_SKY_SRC
    "${ZETA_RP_SKY_DIR}/Sky.cpp"
    "${ZETA_RP_SKY_DIR}/Sky.h"
    "${ZETA_RP_SKY_DIR}/Inscattering.hlsl"
    "${ZETA_RP_SKY_DIR}/SkyViewLUT.hlsl"
    "${ZETA_RP_SKY_DIR}/SkyDome.cpp"
    "${ZETA_RP_SKY_DIR}/SkyDome.h"
    "${ZETA_RP_SKY_DIR}/SkyDome.hlsl"
    "${ZETA_RP_SKY_DIR}/Sky_Common.h")
set(ZETA_RP_SKY_SRC ${ZETA_RP_SKY_SRC} PARENT_SCOPE)