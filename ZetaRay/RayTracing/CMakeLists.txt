set(RT_DIR "${RENDERER_DIR}/RayTracing")
set(RT_SRC
    "${RT_DIR}/RtAccelerationStructure.cpp"
    "${RT_DIR}/RtAccelerationStructure.h"
    "${RT_DIR}/Sampler.cpp"
    "${RT_DIR}/Sampler.h")
set(RT_SRC ${RT_SRC} PARENT_SCOPE)