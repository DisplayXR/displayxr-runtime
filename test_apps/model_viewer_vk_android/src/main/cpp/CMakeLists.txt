# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for model_viewer_vk_android — Android port of David's
# displayxr-demo-modelviewer (glTF 2.0 PBR viewer). Reuses the
# cube_handle/gausssplat OpenXR-Android harness + Kooima view math, swaps the
# renderer for model_common's ModelRenderer. glTF-only (USD/OBJ/STL/FBX
# backends dropped for the Android port); tinygltf + glm via FetchContent.

cmake_minimum_required(VERSION 3.22.1)
project(model_viewer_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)          # model_renderer / glm require C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── glTF 2.0 (tinygltf, header-only; bundles nlohmann/json + stb) ─────────
include(FetchContent)
FetchContent_Declare(tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.3)
set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(tinygltf)

# ── glm (math, header-only) ───────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1)
FetchContent_MakeAvailable(glm)

# ── PBR/IBL shaders → SPIR-V header arrays ────────────────────────────────
# var name = <name>_<stage>_data, header = <name>.<stage>.h (matches the
# #includes in model_renderer.cpp). -I shaders for ibl_common.glsl / sky.glsl.
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK")
endif()
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(MV_SHADERS pbr.vert pbr.frag skybox.frag fullscreen.vert
    brdf_lut.frag irradiance.frag prefilter.frag)
set(SHADER_HEADERS "")
foreach(S ${MV_SHADERS})
    string(REPLACE "." "_" VN "${S}")          # pbr.vert -> pbr_vert
    set(HDR "${CMAKE_CURRENT_BINARY_DIR}/${S}.h")
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2 -I${SHADER_DIR}
            ${SHADER_DIR}/${S} --vn ${VN}_data -o ${HDR}
        DEPENDS ${SHADER_DIR}/${S}
                ${SHADER_DIR}/ibl_common.glsl ${SHADER_DIR}/sky.glsl
        COMMENT "Compiling SPIR-V: ${S}")
    list(APPEND SHADER_HEADERS ${HDR})
endforeach()

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(model_viewer_vk_android SHARED
    main.cpp
    model_renderer.cpp
    model_loader.cpp
    model_loader_gltf.cpp
    model_loader_material.cpp
    model_vulkan_utils.cpp
    stb_impl.cpp
    display3d_view.c
    camera3d_view.c
    ${SHADER_HEADERS})

target_include_directories(model_viewer_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.vert.h / *.frag.h
    ${tinygltf_SOURCE_DIR}                      # tiny_gltf.h + json.hpp + stb_image.h
    ${glm_SOURCE_DIR}                           # glm/ (header-only, no link target needed)
    # DisplayXR custom OpenXR extension headers (XR_EXT_display_info: display
    # rendering-mode enumerate/request). cpp dir is test_apps/<app>/src/main/cpp.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src/external/openxr_includes)

set_target_properties(model_viewer_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(model_viewer_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    android
    log)
