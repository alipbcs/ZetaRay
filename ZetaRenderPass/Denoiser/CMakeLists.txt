set(RP_DENOISER_DIR "${ZETA_RENDER_PASS_DIR}/Denoiser")
set(RP_DENOISER_SRC
    "${RP_DENOISER_DIR}/DiffuseDNSR.cpp"
    "${RP_DENOISER_DIR}/DiffuseDNSR.h"
    "${RP_DENOISER_DIR}/DiffuseDNSR_TemporalPass.hlsl"
    "${RP_DENOISER_DIR}/DiffuseDNSR_SpatialFilter.hlsl"
    "${RP_DENOISER_DIR}/DiffuseDNSR_Common.h")
set(RP_DENOISER_SRC ${RP_DENOISER_SRC} PARENT_SCOPE)