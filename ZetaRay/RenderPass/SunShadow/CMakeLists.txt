set(RP_SUN_DIR "${RENDERPASS_DIR}/SunShadow")
set(RP_SUN_SRC
    "${RP_SUN_DIR}/SunShadow.cpp"
    "${RP_SUN_DIR}/SunShadow.h"
    "${RP_SUN_DIR}/SunShadow_Common.h"
    "${RP_SUN_DIR}/SunShadow.hlsl")
set(RP_SUN_SRC ${RP_SUN_SRC} PARENT_SCOPE)