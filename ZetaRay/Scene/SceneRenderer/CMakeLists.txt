set(SCENERENDERER_DIR "${SCENE_DIR}/SceneRenderer")
set(SCENERENDERER_SRC
    "${SCENERENDERER_DIR}/GBufferRenderer.cpp"
    "${SCENERENDERER_DIR}/Light.cpp"
    "${SCENERENDERER_DIR}/PostProcessor.cpp"
    "${SCENERENDERER_DIR}/RayTracer.cpp"
    "${SCENERENDERER_DIR}/SceneRenderer.cpp"
    "${SCENERENDERER_DIR}/SceneRenderer.h"
    "${SCENERENDERER_DIR}/SceneRendererImpl.h")
    
set(SCENERENDERER_SRC ${SCENERENDERER_SRC} PARENT_SCOPE)