set(RP_AUTO_EXPOSURE_DIR ${ZETA_RENDER_PASS_DIR}/AutoExposure)
set(RP_AUTO_EXPOSURE_SRC
    ${RP_AUTO_EXPOSURE_DIR}/AutoExposure.cpp
    ${RP_AUTO_EXPOSURE_DIR}/AutoExposure.h
    ${RP_AUTO_EXPOSURE_DIR}/AutoExposure_Histogram.hlsl
    ${RP_AUTO_EXPOSURE_DIR}/AutoExposure_WeightedAvg.hlsl
    ${RP_AUTO_EXPOSURE_DIR}/AutoExposure_Common.h)
set(RP_AUTO_EXPOSURE_SRC ${RP_AUTO_EXPOSURE_SRC} PARENT_SCOPE)