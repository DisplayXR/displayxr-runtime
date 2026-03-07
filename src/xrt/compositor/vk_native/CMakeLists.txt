# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# Native Vulkan compositor library
#

if(WIN32 AND XRT_HAVE_VULKAN)
	add_library(
		comp_vk_native STATIC
		comp_vk_native_compositor.c
		comp_vk_native_compositor.h
		comp_vk_native_renderer.c
		comp_vk_native_renderer.h
		comp_vk_native_swapchain.c
		comp_vk_native_swapchain.h
		comp_vk_native_target.c
		comp_vk_native_target.h
		)

	target_link_libraries(
		comp_vk_native
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			comp_win32_window
			aux_vk
			aux_util
			aux_os
			aux_math
		)

	target_include_directories(comp_vk_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	# Qwerty include path for runtime-side 2D/3D toggle polling
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_vk_native PRIVATE drv_qwerty_includes)
		target_compile_definitions(comp_vk_native PRIVATE XRT_BUILD_DRIVER_QWERTY)
	endif()
endif()
