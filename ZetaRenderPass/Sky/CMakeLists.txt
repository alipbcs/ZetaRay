set(RP_SKY_DIR ${ZETA_RENDER_PASS_DIR}/Sky)
set(RP_SKY_SRC
    "${RP_SKY_DIR}/Sky.cpp"
    "${RP_SKY_DIR}/Sky.h"
    "${RP_SKY_DIR}/Inscattering.hlsl"
    "${RP_SKY_DIR}/SkyViewLUT.hlsl"
    "${RP_SKY_DIR}/SkyDome.cpp"
    "${RP_SKY_DIR}/SkyDome.h"
    "${RP_SKY_DIR}/SkyDome.hlsl"
    "${RP_SKY_DIR}/Sky_Common.h")
set(RP_SKY_SRC ${RP_SKY_SRC} PARENT_SCOPE)