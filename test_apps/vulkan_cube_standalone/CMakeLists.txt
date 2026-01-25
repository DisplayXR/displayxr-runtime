# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(vulkan_cube_standalone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Find Vulkan SDK
find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_INCLUDE_DIRS}")

# Find glslc compiler for shader compilation
find_program(GLSLC_EXECUTABLE glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/bin"
)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Please install Vulkan SDK.")
endif()
message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")

# Use shaders from the vulkan test app
set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../session_target_test_vulkan/shaders")
set(SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

set(SHADER_SOURCES
    ${SHADER_SOURCE_DIR}/cube.vert
    ${SHADER_SOURCE_DIR}/cube.frag
)

set(SHADER_OUTPUTS)
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SHADER_OUTPUT "${SHADER_BINARY_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC_EXECUTABLE} -o ${SHADER_OUTPUT} ${SHADER}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

add_custom_target(shaders_standalone DEPENDS ${SHADER_OUTPUTS})

add_executable(vulkan_cube_standalone WIN32
    main.cpp
)

add_dependencies(vulkan_cube_standalone shaders_standalone)

target_include_directories(vulkan_cube_standalone PRIVATE
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(vulkan_cube_standalone PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
)

target_link_libraries(vulkan_cube_standalone PRIVATE
    Vulkan::Vulkan
)

# Pass shader directory to source code
target_compile_definitions(vulkan_cube_standalone PRIVATE
    SHADER_DIR="${SHADER_BINARY_DIR}"
)

# Copy compiled shaders to output directory
add_custom_command(TARGET vulkan_cube_standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vulkan_cube_standalone>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHADER_OUTPUTS}
        $<TARGET_FILE_DIR:vulkan_cube_standalone>/shaders/
    COMMENT "Copying compiled shaders"
)
