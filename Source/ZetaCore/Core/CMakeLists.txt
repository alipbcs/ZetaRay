set(CORE_DIR "${ZETA_CORE_DIR}/Core")
set(CORE_SRC
    "${CORE_DIR}/CommandList.cpp"
    "${CORE_DIR}/CommandList.h"
    "${CORE_DIR}/CommandQueue.cpp"
    "${CORE_DIR}/CommandQueue.h"
    "${CORE_DIR}/Config.h"
    "${CORE_DIR}/dds.h"
    "${CORE_DIR}/DescriptorHeap.cpp"
    "${CORE_DIR}/DescriptorHeap.h"
    "${CORE_DIR}/Device.cpp"
    "${CORE_DIR}/Device.h"
    "${CORE_DIR}/Direct3DUtil.cpp"
    "${CORE_DIR}/Direct3DUtil.h"
    "${CORE_DIR}/GpuMemory.cpp"
    "${CORE_DIR}/GpuMemory.h"
    "${CORE_DIR}/GpuTimer.cpp"
    "${CORE_DIR}/GpuTimer.h"
    "${CORE_DIR}/HLSLCompat.h"
    "${CORE_DIR}/Material.h"
    "${CORE_DIR}/PipelineStateLibrary.cpp"
    "${CORE_DIR}/PipelineStateLibrary.h"
    "${CORE_DIR}/RendererCore.cpp"
    "${CORE_DIR}/RendererCore.h"
    "${CORE_DIR}/RenderGraph.cpp"
    "${CORE_DIR}/RenderGraph.h"
    "${CORE_DIR}/RootSignature.cpp"
    "${CORE_DIR}/RootSignature.h"
    "${CORE_DIR}/SharedShaderResources.cpp"
    "${CORE_DIR}/SharedShaderResources.h"
    "${CORE_DIR}/Vertex.h")
set(CORE_SRC ${CORE_SRC} PARENT_SCOPE)