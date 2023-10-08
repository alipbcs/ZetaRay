add_subdirectory(Emissive)
add_subdirectory(Sky)

set(RP_DI_SRC ${RP_EMISSIVE_DI_SRC} ${RP_SKY_DI_SRC} PARENT_SCOPE)