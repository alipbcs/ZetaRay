set(RP_SKY_DI_DIR ${ZETA_RENDER_PASS_DIR}/Sky/DirectLighting)
set(RP_SKY_DI_SRC
	${RP_SKY_DI_DIR}/SkyDI.cpp
    ${RP_SKY_DI_DIR}/SkyDI.h
    ${RP_SKY_DI_DIR}/SkyDI_Common.h
    ${RP_SKY_DI_DIR}/SkyDI_Reservoir.hlsli
    ${RP_SKY_DI_DIR}/SkyDI_Temporal.hlsl
    ${RP_SKY_DI_DIR}/SkyDI_Spatial.hlsl
    ${RP_SKY_DI_DIR}/SkyDI_DNSR_Temporal.hlsl
    ${RP_SKY_DI_DIR}/SkyDI_DNSR_SpatialFilter.hlsl)
set(RP_SKY_DI_SRC ${RP_SKY_DI_SRC} PARENT_SCOPE)