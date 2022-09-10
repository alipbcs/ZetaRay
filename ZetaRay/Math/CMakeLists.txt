set(ZETA_MATH_DIR "${ZETA_RENDERER_DIR}/Math")
set(ZETA_MATH_SRC
    "${ZETA_MATH_DIR}/BVH.cpp"
    "${ZETA_MATH_DIR}/BVH.h"
    "${ZETA_MATH_DIR}/CollisionFuncs.h"
    "${ZETA_MATH_DIR}/CollisionTypes.h"
    "${ZETA_MATH_DIR}/Common.cpp"
    "${ZETA_MATH_DIR}/Common.h"
    "${ZETA_MATH_DIR}/Matrix.h"
    "${ZETA_MATH_DIR}/MatrixFuncs.h"
    "${ZETA_MATH_DIR}/Quaternion.h"
    "${ZETA_MATH_DIR}/Sampling.cpp"
    "${ZETA_MATH_DIR}/Sampling.h"
    "${ZETA_MATH_DIR}/Surface.cpp"
    "${ZETA_MATH_DIR}/Surface.h"
    "${ZETA_MATH_DIR}/Vector.h"
    "${ZETA_MATH_DIR}/VectorFuncs.h")
set(ZETA_MATH_SRC ${ZETA_MATH_SRC} PARENT_SCOPE)