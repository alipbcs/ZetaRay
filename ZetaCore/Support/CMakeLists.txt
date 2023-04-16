set(SUPPORT_DIR "${ZETA_CORE_DIR}/Support")
set(SUPPORT_SRC
    "${SUPPORT_DIR}/FrameMemory.h"
    "${SUPPORT_DIR}/Memory.h"
    "${SUPPORT_DIR}/MemoryPool.cpp"
    "${SUPPORT_DIR}/MemoryPool.h"
    "${SUPPORT_DIR}/MemoryArena.cpp"
    "${SUPPORT_DIR}/MemoryArena.h"
    "${SUPPORT_DIR}/Param.cpp"
    "${SUPPORT_DIR}/Param.h"
    "${SUPPORT_DIR}/Stat.h"
    "${SUPPORT_DIR}/Task.cpp"
    "${SUPPORT_DIR}/Task.h"
    "${SUPPORT_DIR}/ThreadPool.cpp"
    "${SUPPORT_DIR}/ThreadPool.h"
    "${SUPPORT_DIR}/ThreadSafeMemoryArena.h"
    "${SUPPORT_DIR}/ThreadSafeMemoryArena.cpp")
set(SUPPORT_SRC ${SUPPORT_SRC} PARENT_SCOPE)