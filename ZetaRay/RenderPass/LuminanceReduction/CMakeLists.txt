set(RP_LUM_REDUCTION_DIR ${RENDERPASS_DIR}/LuminanceReduction)
set(RP_LUM_REDUCTION_SRC
    ${RP_LUM_REDUCTION_DIR}/LuminanceReduction.cpp
    ${RP_LUM_REDUCTION_DIR}/LuminanceReduction.h
    ${RP_LUM_REDUCTION_DIR}/ReductionFirst.hlsl
    ${RP_LUM_REDUCTION_DIR}/ReductionSecond.hlsl
    ${RP_LUM_REDUCTION_DIR}/Reduction_Common.h)
set(RP_LUM_REDUCTION_SRC ${RP_LUM_REDUCTION_SRC} PARENT_SCOPE)