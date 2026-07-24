cmake_minimum_required(VERSION 3.14)
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
set(JETSON_ASSETS_DIR "assets" CACHE STRING "Runtime assets directory (relative to CWD or absolute)")

# ----------------------------------------------------------------------------
# Find host libs
# ----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# dlopen/dlclose: on glibc >= 2.34 libdl is part of libc, so the standalone
# libdl may not exist. find_library (not find_package) avoids the noisy
# "no FindDL.cmake" warning and leaves the link line empty when not needed.
find_library(DL_LIBRARY dl)

# libcurl: HTTP client for the in-process LLM (Ollama cloud chat completions),
# Exa web search, the open-meteo weather client and Zing artwork downloads.
find_package(CURL REQUIRED)

# SHA-256 + HMAC-SHA512 signatures for the native Zing music client.
find_package(OpenSSL REQUIRED)

# ALSA: mic capture + TTS/ringtone playback on the reSpeaker USB v3 (UAC2).
find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
find_library(ALSA_LIBRARY NAMES asound)
if(NOT ALSA_LIBRARY)
    message(FATAL_ERROR "ALSA (libasound2) not found. Run scripts/fetch_deps.sh (apt: libasound2-dev).")
endif()

# ----------------------------------------------------------------------------
# sherpa-onnx: wake-word (KWS) + streaming STT + offline TTS.
#
# Two ways to link it, in priority order:
#   1. JETSON_SHERPA_DIR set -> link a prebuilt install prefix
#      (lib/libsherpa-onnx.* + include/sherpa-onnx/c-api/c-api.h). This is the
#      path scripts/fetch_sherpa.sh produces: it builds sherpa-onnx once with
#      GPU + onnxruntime 1.11 CUDA into a prefix, then every firmware rebuild
#      reuses it without recompiling sherpa. Fastest inner loop.
#   2. JETSON_BUILD_SHERPA=ON (opt-in; OFF by default) -> FetchContent the
#      pinned tag and add_subdirectory it. Slower (rebuilds sherpa with the
#      firmware) and pulls sherpa's own dep tree (onnxruntime/k2/asio/...), so
#      it is NOT the default -- prefer the prebuilt prefix above, or leave both
#      off to build a stub voice engine (loop inert, agent still fully usable).
# Either way JETSON_HAVE_SHERPA is defined so the code knows the C API is there.
# ----------------------------------------------------------------------------
set(JETSON_SHERPA_DIR "" CACHE PATH "Prebuilt sherpa-onnx install prefix")
set(JETSON_SHERPA_TAG "v1.10.30" CACHE STRING "sherpa-onnx git tag (FetchContent path)")
option(JETSON_BUILD_SHERPA "Build sherpa-onnx from source via FetchContent" OFF)

if(JETSON_SHERPA_DIR)
    set(SHERPA_INCLUDE_DIR "${JETSON_SHERPA_DIR}/include")
    # The prebuilt/shared prefix ships libsherpa-onnx-c-api.so (the C API); a
    # from-source install may ship libsherpa-onnx.a. Accept either name/ext.
    find_library(SHERPA_LIBRARY
        NAMES sherpa-onnx-c-api sherpa-onnx
        PATHS "${JETSON_SHERPA_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT SHERPA_LIBRARY)
        message(FATAL_ERROR "JETSON_SHERPA_DIR set to ${JETSON_SHERPA_DIR} but no "
            "libsherpa-onnx-c-api / libsherpa-onnx found under ${JETSON_SHERPA_DIR}/lib")
    endif()
    set(JETSON_HAVE_SHERPA ON)
elseif(JETSON_BUILD_SHERPA)
    include(FetchContent)
    FetchContent_Declare(sherpa_onnx
        GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
        GIT_TAG ${JETSON_SHERPA_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(sherpa_onnx)
    if(NOT sherpa_onnx_POPULATED)
        FetchContent_Populate(sherpa_onnx)
    endif()
    # GPU EP for STT (user asked for GPU). CUDA 10.2 / cuDNN 8 must be present
    # on the Jetson (JetPack 4.6). sherpa's own CMake downloads a matching
    # onnxruntime build for the platform.
    set(SHERPA_ONNX_ENABLE_GPU ON CACHE BOOL "" FORCE)
    add_subdirectory(${sherpa_onnx_SOURCE_DIR} ${sherpa_onnx_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(SHERPA_INCLUDE_DIR "${sherpa_onnx_SOURCE_DIR}")
    # sherpa-onnx defines `sherpa-onnx` as an *executable* (the CLI), which CMake
    # refuses to link into jetson_fw. The linkable C-API library target is
    # `sherpa-onnx-c-api` (static); it transitively pulls sherpa-onnx-core +
    # onnxruntime, so no extra link line is needed on this path.
    set(SHERPA_LIBRARY sherpa-onnx-c-api)
    set(JETSON_HAVE_SHERPA ON)
endif()

# ----------------------------------------------------------------------------
# Firmware (headless "Ekko Lite" — no display, no LVGL)
# ----------------------------------------------------------------------------
file(GLOB_RECURSE FW_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(jetson_fw ${FW_SOURCES})

# Keep the source root for boundary-qualified includes (`display/core/...`,
# `platform/...`) and the first-level module directories for the legacy ported
# headers that still use short include names.
file(GLOB _fw_subdirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/src/*")
set(FW_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src")
foreach(_d ${_fw_subdirs})
    if(IS_DIRECTORY "${_d}")
        list(APPEND FW_INCLUDE_DIRS "${_d}")
    endif()
endforeach()

target_include_directories(jetson_fw PRIVATE ${FW_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${ALSA_INCLUDE_DIR})

target_compile_definitions(jetson_fw PRIVATE
    JETSON_ASSETS_DIR="${JETSON_ASSETS_DIR}"
    JETSON_FW_VERSION="${PROJECT_VERSION}"
    BOARD_NAME="jetson-nano")

if(JETSON_HAVE_SHERPA)
    target_include_directories(jetson_fw PRIVATE ${SHERPA_INCLUDE_DIR})
    target_compile_definitions(jetson_fw PRIVATE JETSON_HAVE_SHERPA=1)
endif()

target_link_libraries(jetson_fw PRIVATE
    Threads::Threads
    ${DL_LIBRARY}
    CURL::libcurl
    OpenSSL::Crypto
    ${ALSA_LIBRARY})

if(JETSON_HAVE_SHERPA)
    # sherpa links (shared prebuilt or static from-source). The shared .so has
    # NEEDED onnxruntime/cargs recorded, so linking the c-api lib alone suffices
    # and the runtime loader pulls the rest from the same dir (via RPATH below).
    # For a static build, add onnxruntime (+ CUDA/cuDNN) via JETSON_SHERPA_EXTRA_LIBS.
    target_link_libraries(jetson_fw PRIVATE ${SHERPA_LIBRARY})
    if(JETSON_SHERPA_EXTRA_LIBS)
        target_link_libraries(jetson_fw PRIVATE ${JETSON_SHERPA_EXTRA_LIBS})
    endif()
    # Bake the prefix lib dir into RPATH so jetson_fw finds libsherpa-onnx-c-api /
    # libonnxruntime at runtime without LD_LIBRARY_PATH.
    get_filename_component(SHERPA_LIB_DIR "${SHERPA_LIBRARY}" DIRECTORY)
    set_target_properties(jetson_fw PROPERTIES
        BUILD_RPATH "${SHERPA_LIB_DIR}"
        INSTALL_RPATH "${SHERPA_LIB_DIR}")
endif()

# forkpty() lives in libutil on older glibc (e.g. JetPack 4.x / Ubuntu 18.04,
# glibc 2.27). On glibc >= 2.34 it is part of libc, so the library is optional.
find_library(UTIL_LIBRARY util)
if(UTIL_LIBRARY)
    target_link_libraries(jetson_fw PRIVATE ${UTIL_LIBRARY})
endif()

# ----------------------------------------------------------------------------
# Runtime assets: fetch from MinIO (S3) before the build, then copy next to
# the binary. Ekko Lite keeps the S3 sync so ringtones (and any other audio
# assets) can be pushed to the bucket and land on the device at build time.
# fetch_assets.sh is idempotent -- it skips files already present -- so
# rebuilds are cheap. It is also offline-tolerant: if the server is unreachable
# but assets are already cached it keeps them and lets the build proceed.
# See scripts/fetch_assets.sh + scripts/s3_assets.py.
find_program(PYTHON3_EXE NAMES python3 python)
find_program(BASH_EXE NAMES bash)
if(PYTHON3_EXE AND BASH_EXE)
    add_custom_target(jetson_fetch_assets ALL
        COMMAND "${BASH_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch_assets.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Fetching runtime assets from MinIO (skipping files already present)"
        VERBATIM
        USES_TERMINAL)
    add_dependencies(jetson_fw jetson_fetch_assets)
else()
    message(WARNING "python3 or bash not found; skipping build-time asset fetch. "
        "Run scripts/fetch_assets.sh manually before building, or assets/ will be empty.")
endif()

# Copy assets next to the binary on every build, including incremental builds
# where jetson_fw does not need to be relinked. A POST_BUILD command alone
# leaves newly-added assets out of an existing build directory until some C/C++
# source happens to change.
add_custom_target(jetson_copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:jetson_fw>/assets")
add_dependencies(jetson_copy_assets jetson_fw)

# ----------------------------------------------------------------------------
# Install (systemd boot service -> /opt/jetson-fw)
# ----------------------------------------------------------------------------
install(TARGETS jetson_fw RUNTIME DESTINATION /opt/jetson-fw)
install(DIRECTORY assets DESTINATION /opt/jetson-fw)
install(FILES config.yaml DESTINATION /opt/jetson-fw)
install(PROGRAMS scripts/s3_assets.py scripts/config_loader.sh
        DESTINATION /opt/jetson-fw/scripts)
install(PROGRAMS scripts/jetson-fw.service DESTINATION /etc/systemd/system OPTIONAL)
message(STATUS "jetson_fw: headless Ekko Lite build")