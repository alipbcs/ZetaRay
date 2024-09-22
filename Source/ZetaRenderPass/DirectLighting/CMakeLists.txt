set(RP_SKY_DI_DIR ${ZETA_RENDER_PASS_DIR}/DirectLighting/Sky)
set(RP_EMISSIVE_DI_DIR ${ZETA_RENDER_PASS_DIR}/DirectLighting/Emissive)
set(RP_DI_SRC
    ${RP_SKY_DI_DIR}/PairwiseMIS.hlsli
    ${RP_SKY_DI_DIR}/Params.hlsli
    ${RP_SKY_DI_DIR}/Resampling.hlsli
    ${RP_SKY_DI_DIR}/Reservoir.hlsli
	${RP_SKY_DI_DIR}/SkyDI.cpp
    ${RP_SKY_DI_DIR}/SkyDI.h
    ${RP_SKY_DI_DIR}/SkyDI.hlsl
    ${RP_SKY_DI_DIR}/SkyDI_Common.h
    ${RP_SKY_DI_DIR}/SkyDI_DNSR_Temporal.hlsl
    ${RP_SKY_DI_DIR}/SkyDI_DNSR_Spatial.hlsl
    ${RP_EMISSIVE_DI_DIR}/DirectLighting.cpp
    ${RP_EMISSIVE_DI_DIR}/DirectLighting.h
    ${RP_EMISSIVE_DI_DIR}/DirectLighting_Common.h
    ${RP_EMISSIVE_DI_DIR}/PairwiseMIS.hlsli
    ${RP_EMISSIVE_DI_DIR}/Params.hlsli
    ${RP_EMISSIVE_DI_DIR}/Resampling.hlsli
    ${RP_EMISSIVE_DI_DIR}/Reservoir.hlsli
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_Emissive.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_Emissive_WPS.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_DNSR_Temporal.hlsl
    ${RP_EMISSIVE_DI_DIR}/ReSTIR_DI_DNSR_Spatial.hlsl
    ${RP_EMISSIVE_DI_DIR}/Util.hlsli)
set(RP_DI_SRC ${RP_DI_SRC} PARENT_SCOPE)