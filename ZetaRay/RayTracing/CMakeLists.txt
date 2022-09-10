set(ZETA_RT_DIR "${ZETA_RENDERER_DIR}/RayTracing")
set(ZETA_RT_SRC
    "${ZETA_RT_DIR}/RtAccelerationStructure.cpp"
    "${ZETA_RT_DIR}/RtAccelerationStructure.h"
    "${ZETA_RT_DIR}/Sampler.cpp"
    "${ZETA_RT_DIR}/Sampler.h")
set(ZETA_RT_SRC ${ZETA_RT_SRC} PARENT_SCOPE)