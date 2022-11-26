set(SCENE_DIR "${RENDERER_DIR}/Scene")
set(SCENE_SRC
    "${SCENE_DIR}/Asset.cpp"
    "${SCENE_DIR}/Asset.h"
    "${SCENE_DIR}/Camera.cpp"
    "${SCENE_DIR}/Camera.h"
    "${SCENE_DIR}/SceneCore.cpp"
    "${SCENE_DIR}/SceneCore.h")
    
add_subdirectory(SceneRenderer)
set(SCENE_SRC ${SCENE_SRC} ${SCENERENDERER_SRC} PARENT_SCOPE)