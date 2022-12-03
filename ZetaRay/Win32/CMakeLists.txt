set(WIN32_DIR "${RENDERER_DIR}/Win32")
set(WIN32_SRC
    "${WIN32_DIR}/Win32App.cpp"
    "${WIN32_DIR}/Win32Filesystem.cpp"
    "${WIN32_DIR}/Win32Timer.cpp"
    "${WIN32_DIR}/Win32.h"
    "${WIN32_DIR}/Win32Common.cpp")
set(WIN32_SRC ${WIN32_SRC} PARENT_SCOPE)