set(ZETA_UTIL_DIR "${ZETA_RENDERER_DIR}/Utility")
set(ZETA_UTIL_SRC
    "${ZETA_UTIL_DIR}/Error.cpp"
    "${ZETA_UTIL_DIR}/Error.h"
    "${ZETA_UTIL_DIR}/Function.h"
    "${ZETA_UTIL_DIR}/HashTable.h"
    "${ZETA_UTIL_DIR}/RNG.h"
    "${ZETA_UTIL_DIR}/SmallVector.h"
    "${ZETA_UTIL_DIR}/Span.h"
    "${ZETA_UTIL_DIR}/SynchronizedView.h")
set(ZETA_UTIL_SRC ${ZETA_UTIL_SRC} PARENT_SCOPE)