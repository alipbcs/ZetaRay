set(RP_SUN_DIR ${ZETA_RENDER_PASS_DIR}/SunShadow)
set(RP_SUN_SRC
    "${RP_SUN_DIR}/SunShadow.cpp"
    "${RP_SUN_DIR}/SunShadow.h"
    "${RP_SUN_DIR}/SunShadow_Common.h"
    "${RP_SUN_DIR}/SunShadow.hlsl"
    "${RP_SUN_DIR}/ffx_denoiser_util.hlsli"
    "${RP_SUN_DIR}/ffx_denoiser_temporal.hlsl"
    "${RP_SUN_DIR}/ffx_denoiser_spatial_filter.hlsl")
set(RP_SUN_SRC ${RP_SUN_SRC} PARENT_SCOPE)