set(ZETA_SCENE_DIR "${ZETA_RENDERER_DIR}/Scene")
set(ZETA_SCENE_SRC
    "${ZETA_SCENE_DIR}/AssetManager.cpp"
    "${ZETA_SCENE_DIR}/AssetManager.h"
    "${ZETA_SCENE_DIR}/Assets.h"
    "${ZETA_SCENE_DIR}/Camera.cpp"
    "${ZETA_SCENE_DIR}/Camera.h"
    "${ZETA_SCENE_DIR}/SceneCore.cpp"
    "${ZETA_SCENE_DIR}/SceneCore.h")
    
add_subdirectory(SceneRenderer)
set(ZETA_SCENE_SRC ${ZETA_SCENE_SRC} ${ZETA_SCENERENDERER_SRC} PARENT_SCOPE)