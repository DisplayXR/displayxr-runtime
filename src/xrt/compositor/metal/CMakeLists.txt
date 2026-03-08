# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# Native Metal compositor library
#

if(APPLE AND XRT_HAVE_METAL)
	add_library(
		comp_metal STATIC
		comp_metal_compositor.m
		comp_metal_compositor.h
		${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_hud_overlay_macos.m
		${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_hud_overlay_macos.h
		)

	target_link_libraries(
		comp_metal
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			aux_util
			aux_os
			aux_math
			"-framework Metal"
			"-framework Cocoa"
			"-framework QuartzCore"
			"-framework IOSurface"
		)

	target_include_directories(comp_metal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	# Qwerty include path for runtime-side 2D/3D toggle polling
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_metal PRIVATE drv_qwerty_includes)
		target_compile_definitions(comp_metal PRIVATE XRT_BUILD_DRIVER_QWERTY)
	endif()

	# Note: ARC is NOT used because os_threading.h semaphore inlines use
	# dispatch_release which is forbidden under ARC. Metal object lifetimes
	# are managed explicitly with retain/release.
endif()
