# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# Native OpenGL compositor library
#

add_library(
	comp_gl STATIC
	comp_gl_compositor.c
	comp_gl_compositor.h
	# comp_layer_accum has no Vulkan deps — compile directly to avoid pulling in comp_util
	${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_layer_accum.c
	${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_layer_accum.h
	)

target_link_libraries(
	comp_gl
	PUBLIC xrt-interfaces
	PRIVATE
		aux_util
		aux_os
		aux_math
		aux_ogl
	)

target_include_directories(comp_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# comp_layer_accum lives in util/ — include path needed
target_include_directories(comp_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../util)

# aux_ogl headers (ogl_api.h, wgl_api.h) are in auxiliary/ogl/
target_include_directories(comp_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../auxiliary)

# Silence macOS deprecation warnings for OpenGL
if(APPLE)
	target_compile_definitions(comp_gl PRIVATE GL_SILENCE_DEPRECATION)
	target_sources(comp_gl PRIVATE comp_gl_window_macos.h comp_gl_window_macos.m
		${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_hud_overlay_macos.m
		${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_hud_overlay_macos.h)
endif()

# Platform-specific linking
if(WIN32)
	target_link_libraries(comp_gl PRIVATE opengl32 d3d11)
elseif(APPLE)
	target_link_libraries(comp_gl PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework IOSurface")
endif()

# Qwerty include path for runtime-side 2D/3D toggle polling
if(XRT_BUILD_DRIVER_QWERTY)
	target_link_libraries(comp_gl PRIVATE drv_qwerty_includes)
	target_compile_definitions(comp_gl PRIVATE XRT_BUILD_DRIVER_QWERTY)
endif()
