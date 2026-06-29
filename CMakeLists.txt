cmake_minimum_required(VERSION 3.16)
project(xr-workspace VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Version string: overridable by CI (-DXR_VERSION=x.y.z); defaults to VERSION file.
if(NOT DEFINED XR_VERSION)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" XR_VERSION LIMIT_COUNT 1)
        string(STRIP "${XR_VERSION}" XR_VERSION)
    else()
        set(XR_VERSION "${PROJECT_VERSION}")
    endif()
endif()
add_compile_definitions(XR_VERSION="${XR_VERSION}")
message(STATUS "xr-workspace version: ${XR_VERSION}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)

# gbm enables the zero-copy dmabuf capture path (EGLImage import). Optional:
# without it we still build and fall back to the shm capture path at runtime.
pkg_check_modules(GBM gbm)
if(GBM_FOUND)
    message(STATUS "gbm ${GBM_VERSION} found — enabling dmabuf zero-copy capture")
else()
    message(STATUS "gbm not found — dmabuf capture disabled, shm fallback only")
endif()

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Generate protocol sources from bundled XMLs
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

macro(gen_protocol name)
    set(xml "${PROTO_DIR}/${name}.xml")
    set(hdr "${PROTO_GEN_DIR}/${name}-client.h")
    set(src "${PROTO_GEN_DIR}/${name}-client.c")
    add_custom_command(
        OUTPUT ${hdr} ${src}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${hdr}
        COMMAND ${WAYLAND_SCANNER} private-code  ${xml} ${src}
        # Rename C++ reserved keywords used as parameter names in generated headers
        COMMAND sed -i "s/\\bnamespace\\b/wl_namespace/g" ${hdr}
        DEPENDS ${xml}
        VERBATIM
    )
    list(APPEND PROTO_SRCS ${hdr} ${src})
    # Mark protocol C files explicitly as C so C++ reserved words don't conflict
    set_source_files_properties(${src} PROPERTIES LANGUAGE C)
endmacro()

gen_protocol(wlr-layer-shell-unstable-v1)
gen_protocol(xdg-output-unstable-v1)
gen_protocol(hyprland-global-shortcuts-v1)
gen_protocol(linux-dmabuf-unstable-v1)
gen_protocol(wlr-screencopy-unstable-v1)
gen_protocol(ext-image-capture-source-v1)
gen_protocol(ext-image-copy-capture-v1)

add_executable(xr-workspace
    src/main.cpp
    src/pose_breezy.cpp
    src/capture_solid.cpp
    src/capture_shm.cpp
    src/capture_wlr.cpp
    src/capture_ext.cpp
    src/capture_factory.cpp
    src/egl_dmabuf.cpp
    src/hypr_ipc.cpp
    src/json.cpp
    src/config.cpp
    src/control.cpp
    src/sink_opentrack.cpp
    ${PROTO_SRCS}
)

if(GBM_FOUND)
    target_compile_definitions(xr-workspace PRIVATE HAVE_GBM=1)
endif()

target_include_directories(xr-workspace PRIVATE
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLESV2_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
    src
)

target_link_libraries(xr-workspace PRIVATE
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLESV2_LIBRARIES}
    ${GBM_LIBRARIES}
    m
)

target_compile_options(xr-workspace PRIVATE -Wall -Wextra)
