# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for mediaplayer_vk_android — Android port of David's
# displayxr-demo-mediaplayer (stereo SBS image/video player). Reuses the
# cube_handle/model_viewer OpenXR-Android harness; the renderer is a minimal
# SBS blit (fullscreen.vert + sbs.frag), one UV half per eye. Stage 1 = SBS
# image (stb, mode 0). Stage 2 will add AMediaCodec video → the shader's YUV
# path (mode 1/2) via libmediandk.

cmake_minimum_required(VERSION 3.22.1)
project(mediaplayer_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── SBS blit shaders → SPIR-V header arrays ───────────────────────────────
# var name = <name>_<stage>_data, header = <name>.<stage>.h (matches the
# #includes in sbs_renderer.cpp).
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK")
endif()
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(MP_SHADERS fullscreen.vert sbs.frag)
set(SHADER_HEADERS "")
foreach(S ${MP_SHADERS})
    string(REPLACE "." "_" VN "${S}")          # fullscreen.vert -> fullscreen_vert
    set(HDR "${CMAKE_CURRENT_BINARY_DIR}/${S}.h")
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            ${SHADER_DIR}/${S} --vn ${VN}_data -o ${HDR}
        DEPENDS ${SHADER_DIR}/${S}
        COMMENT "Compiling SPIR-V: ${S}")
    list(APPEND SHADER_HEADERS ${HDR})
endforeach()

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(mediaplayer_vk_android SHARED
    main.cpp
    sbs_renderer.cpp
    video_decoder.cpp
    audio_player.cpp
    stb_impl.cpp
    display3d_view.c
    camera3d_view.c
    ${SHADER_HEADERS})

target_include_directories(mediaplayer_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.vert.h / *.frag.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stb             # vendored stb_image.h (SBS image decode)
    # DisplayXR custom OpenXR extension headers (XR_EXT_display_info).
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src/external/openxr_includes)

set_target_properties(mediaplayer_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(mediaplayer_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    mediandk   # AMediaExtractor + AMediaCodec (framework-native video decode)
    aaudio     # AAudio output (audio playback / A-V master clock)
    android
    log)
