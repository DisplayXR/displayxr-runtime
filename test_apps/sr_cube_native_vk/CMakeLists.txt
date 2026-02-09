# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(sr_cube_native_vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Find Vulkan SDK
find_package(Vulkan REQUIRED)

# Find LeiaSR SDK
set(LEIASR_SDKROOT "$ENV{LEIASR_SDKROOT}" CACHE PATH "Path to LeiaSR SDK root")

# Add SDK cmake path to prefix
if(LEIASR_SDKROOT)
    list(APPEND CMAKE_PREFIX_PATH "${LEIASR_SDKROOT}/lib/cmake")
    message(STATUS "LeiaSR SDK root: ${LEIASR_SDKROOT}")
endif()

find_package(simulatedreality QUIET)

if(simulatedreality_FOUND)
    message(STATUS "Found LeiaSR SDK - SR native Vulkan app will have full functionality")
    set(HAVE_LEIA_SR TRUE)
else()
    message(WARNING "LeiaSR SDK not found - SR native Vulkan app will show error message at runtime")
    message(STATUS "  Set LEIASR_SDKROOT environment variable to the SDK installation path")
    set(HAVE_LEIA_SR FALSE)
endif()

# Check for Vulkan weaver library
if(HAVE_LEIA_SR AND LEIASR_SDKROOT)
    set(VK_WEAVER_LIB "${LEIASR_SDKROOT}/lib/SimulatedRealityVulkanBeta.lib")
    if(EXISTS "${VK_WEAVER_LIB}")
        message(STATUS "Found SimulatedRealityVulkanBeta.lib")
        set(HAVE_VK_WEAVER TRUE)
    else()
        message(WARNING "SimulatedRealityVulkanBeta.lib not found at ${VK_WEAVER_LIB}")
        set(HAVE_VK_WEAVER FALSE)
    endif()
else()
    set(HAVE_VK_WEAVER FALSE)
endif()

# Common base library (no D3D11 dependency)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)

# Create executable
add_executable(sr_cube_native_vk WIN32
    main.cpp
    vk_renderer.h
    vk_renderer.cpp
    sr_cube_native_vk.manifest
    resource.rc
)

target_link_libraries(sr_cube_native_vk PRIVATE
    sr_common_base
    Vulkan::Vulkan
)

# Add LeiaSR SDK if available
if(HAVE_LEIA_SR)
    target_compile_definitions(sr_cube_native_vk PRIVATE HAVE_LEIA_SR)
    target_link_libraries(sr_cube_native_vk PRIVATE
        simulatedreality
    )

    # Link Vulkan weaver directly by path
    if(HAVE_VK_WEAVER)
        target_compile_definitions(sr_cube_native_vk PRIVATE HAVE_VK_WEAVER)
        target_link_libraries(sr_cube_native_vk PRIVATE
            "${VK_WEAVER_LIB}"
        )
        target_include_directories(sr_cube_native_vk PRIVATE
            "${LEIASR_SDKROOT}/include"
        )
    endif()
endif()

# Embed manifest
if(MSVC)
    set_target_properties(sr_cube_native_vk PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/sr_cube_native_vk.manifest"
    )
endif()

# Copy runtime DLLs to output directory (only if there are DLLs to copy)
if(HAVE_LEIA_SR)
    add_custom_command(TARGET sr_cube_native_vk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E
            "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:sr_cube_native_vk>>,copy_if_different;$<TARGET_RUNTIME_DLLS:sr_cube_native_vk>;$<TARGET_FILE_DIR:sr_cube_native_vk>,true>"
        COMMAND_EXPAND_LISTS
    )
endif()
