cmake_minimum_required(VERSION 3.13)
project(jetson_fw LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
                    -Wno-missing-field-initializers -Wno-sign-compare -Wno-deprecated-declarations)

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
set(JETSON_DISPLAY_BACKEND "DRM" CACHE STRING "Display backend: DRM (KMS, default) or SDL (fallback)")
set_property(CACHE JETSON_DISPLAY_BACKEND PROPERTY STRINGS DRM SDL)

set(JETSON_DISPLAY_WIDTH  800 CACHE STRING "HDMI panel width")
set(JETSON_DISPLAY_HEIGHT 480 CACHE STRING "HDMI panel height")

set(JETSON_ASSETS_DIR "assets" CACHE STRING "Runtime assets directory (relative to CWD or absolute)")

# ----------------------------------------------------------------------------
# Find host libs
# ----------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(DL)

# libcurl: HTTP client for the in-process LLM (Ollama cloud chat completions).
find_package(CURL REQUIRED)

# libdrm is always useful (DRM backend uses it; SDL/kmsdrm also benefits).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(DRM IMPORTED_TARGET libdrm)
endif()
if(NOT DRM_FOUND)
    # Fallback: assume system libdrm
    set(DRM_FOUND TRUE)
    set(DRM_LIBRARIES drm)
    set(DRM_INCLUDE_DIRS "")
endif()

if(JETSON_DISPLAY_BACKEND STREQUAL "SDL")
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    endif()
    if(NOT SDL2_FOUND)
        message(WARNING "SDL2 not found via pkg-config; assuming libsdl2-dev is installed (libs: SDL2).")
        set(SDL2_LIBRARIES SDL2)
        set(SDL2_INCLUDE_DIRS "")
    endif()
endif()

# ----------------------------------------------------------------------------
# LVGL (vendored)
# ----------------------------------------------------------------------------
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/lvgl)

# Link libdrm into LVGL (the DRM driver #includes <xf86drm.h>, <drm/drm.h>).
if(DRM_FOUND)
    target_include_directories(lvgl PRIVATE ${DRM_INCLUDE_DIRS})
    target_link_libraries(lvgl PUBLIC ${DRM_LIBRARIES})
endif()

# Enable SDL driver inside LVGL when chosen.
if(JETSON_DISPLAY_BACKEND STREQUAL "SDL")
    target_compile_definitions(lvgl PUBLIC LV_USE_SDL=1)
    target_include_directories(lvgl PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
endif()

# ----------------------------------------------------------------------------
# Our firmware
# ----------------------------------------------------------------------------
file(GLOB_RECURSE FW_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(jetson_fw ${FW_SOURCES})

# Recursively add every directory under src/ as an include path, so a file in
# src/display/ can `#include "fonts.h"` (src/app/), "lvgl_runtime.h" (src/platform/),
# "wifi_manager.h" (src/net/), etc. without per-dir bookkeeping.
file(GLOB _fw_subdirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/src/*")
set(FW_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src")
foreach(_d ${_fw_subdirs})
    if(IS_DIRECTORY "${_d}")
        list(APPEND FW_INCLUDE_DIRS "${_d}")
    endif()
endforeach()

target_include_directories(jetson_fw PRIVATE ${FW_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(jetson_fw PRIVATE
    JETSON_DISPLAY_BACKEND_${JETSON_DISPLAY_BACKEND}
    JETSON_DISPLAY_BACKEND="${JETSON_DISPLAY_BACKEND}"
    JETSON_DISPLAY_WIDTH=${JETSON_DISPLAY_WIDTH}
    JETSON_DISPLAY_HEIGHT=${JETSON_DISPLAY_HEIGHT}
    JETSON_ASSETS_DIR="${JETSON_ASSETS_DIR}"
    BOARD_NAME="jetson-nano")

target_link_libraries(jetson_fw PRIVATE
    lvgl::lvgl
    Threads::Threads
    ${DL_LIBRARIES}
    ${DRM_LIBRARIES}
    CURL::libcurl)

if(JETSON_DISPLAY_BACKEND STREQUAL "SDL")
    target_link_libraries(jetson_fw PRIVATE ${SDL2_LIBRARIES})
endif()

# Copy assets next to the binary so the default relative ASSETS_DIR works.
add_custom_command(TARGET jetson_fw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:jetson_fw>/assets")

# ----------------------------------------------------------------------------
# Install (systemd boot service -> /opt/jetson-fw)
# ----------------------------------------------------------------------------
install(TARGETS jetson_fw RUNTIME DESTINATION /opt/jetson-fw)
install(DIRECTORY assets DESTINATION /opt/jetson-fw)
install(PROGRAMS scripts/jetson-fw.service DESTINATION /etc/systemd/system OPTIONAL)
message(STATUS "jetson_fw: backend=${JETSON_DISPLAY_BACKEND} ${JETSON_DISPLAY_WIDTH}x${JETSON_DISPLAY_HEIGHT}")