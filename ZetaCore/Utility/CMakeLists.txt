set(UTIL_DIR "${ZETA_CORE_DIR}/Utility")
set(UTIL_SRC
    "${UTIL_DIR}/Error.cpp"
    "${UTIL_DIR}/Error.h"
    "${UTIL_DIR}/Function.h"
    "${UTIL_DIR}/HashTable.h"
    "${UTIL_DIR}/RNG.h"
    "${UTIL_DIR}/SmallVector.h"
    "${UTIL_DIR}/Span.h"
    "${UTIL_DIR}/SynchronizedView.h")
set(UTIL_SRC ${UTIL_SRC} PARENT_SCOPE)