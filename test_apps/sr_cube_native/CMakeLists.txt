# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(sr_cube_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Find LeiaSR SDK
set(LEIASR_SDKROOT "$ENV{LEIASR_SDKROOT}" CACHE PATH "Path to LeiaSR SDK root")

# Add SDK cmake path to prefix
if(LEIASR_SDKROOT)
    list(APPEND CMAKE_PREFIX_PATH "${LEIASR_SDKROOT}/lib/cmake")
    message(STATUS "LeiaSR SDK root: ${LEIASR_SDKROOT}")
endif()

find_package(simulatedreality QUIET)
find_package(srDirectX QUIET)

if(simulatedreality_FOUND AND srDirectX_FOUND)
    message(STATUS "Found LeiaSR SDK - SR native app will have full functionality")
    set(HAVE_LEIA_SR TRUE)
else()
    message(WARNING "LeiaSR SDK not found - SR native app will show error message at runtime")
    message(STATUS "  Set LEIASR_SDKROOT environment variable to the SDK installation path")
    set(HAVE_LEIA_SR FALSE)
endif()

# Common library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)

# Create executable
add_executable(sr_cube_native WIN32
    main.cpp
    sr_cube_native.manifest
    resource.rc
)

target_link_libraries(sr_cube_native PRIVATE
    sr_common
)

# Add LeiaSR SDK if available
if(HAVE_LEIA_SR)
    target_compile_definitions(sr_cube_native PRIVATE XRT_HAVE_CNSDK)
    target_link_libraries(sr_cube_native PRIVATE
        simulatedreality
        srDirectX::srDirectX
    )
endif()

# Embed manifest
if(MSVC)
    set_target_properties(sr_cube_native PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/sr_cube_native.manifest"
    )
endif()

# Copy runtime DLLs to output directory (only if there are DLLs to copy)
if(HAVE_LEIA_SR)
    add_custom_command(TARGET sr_cube_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E
            "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:sr_cube_native>>,copy_if_different;$<TARGET_RUNTIME_DLLS:sr_cube_native>;$<TARGET_FILE_DIR:sr_cube_native>,true>"
        COMMAND_EXPAND_LISTS
    )
endif()
