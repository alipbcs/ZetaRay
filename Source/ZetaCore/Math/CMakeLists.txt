set(MATH_DIR "${ZETA_CORE_DIR}/Math")
set(MATH_SRC
    "${MATH_DIR}/BVH.cpp"
    "${MATH_DIR}/BVH.h"
    "${MATH_DIR}/CollisionFuncs.h"
    "${MATH_DIR}/CollisionTypes.h"
    "${MATH_DIR}/Common.cpp"
    "${MATH_DIR}/Common.h"
    "${MATH_DIR}/Color.h"
    "${MATH_DIR}/Matrix.h"
    "${MATH_DIR}/MatrixFuncs.h"
    "${MATH_DIR}/OctahedralVector.h"
    "${MATH_DIR}/Quaternion.h"
    "${MATH_DIR}/Sampling.cpp"
    "${MATH_DIR}/Sampling.h"
    "${MATH_DIR}/Surface.cpp"
    "${MATH_DIR}/Surface.h"
    "${MATH_DIR}/Vector.h"
    "${MATH_DIR}/VectorFuncs.h")
set(MATH_SRC ${MATH_SRC} PARENT_SCOPE)