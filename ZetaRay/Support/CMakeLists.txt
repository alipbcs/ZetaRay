set(ZETA_SUPPORT_DIR "${ZETA_RENDERER_DIR}/Support")
set(ZETA_SUPPORT_SRC
    "${ZETA_SUPPORT_DIR}/Memory.cpp"
    "${ZETA_SUPPORT_DIR}/Memory.h"
    "${ZETA_SUPPORT_DIR}/Param.cpp"
    "${ZETA_SUPPORT_DIR}/Param.h"
    "${ZETA_SUPPORT_DIR}/Stat.h"
    "${ZETA_SUPPORT_DIR}/Task.cpp"
    "${ZETA_SUPPORT_DIR}/Task.h"
    "${ZETA_SUPPORT_DIR}/ThreadPool.cpp"
    "${ZETA_SUPPORT_DIR}/ThreadPool.h")
set(ZETA_SUPPORT_SRC ${ZETA_SUPPORT_SRC} PARENT_SCOPE)