# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# Native Vulkan compositor library
#

if((WIN32 OR APPLE) AND XRT_HAVE_VULKAN)
	add_library(
		comp_vk_native STATIC
		comp_vk_native_compositor.c
		comp_vk_native_compositor.h
		comp_vk_native_renderer.c
		comp_vk_native_renderer.h
		comp_vk_native_swapchain.c
		comp_vk_native_swapchain.h
		comp_vk_native_target.cpp
		comp_vk_native_target.h
		)

	if(WIN32)
		# DComp + WGL_NV_DX_interop2 dependencies for the VK->D3D11 transparent
		# present bridge. dcomp.h is C++-only, hence the .cpp rename above.
		target_link_libraries(comp_vk_native PRIVATE d3d11 dxgi dcomp)
	endif()

	target_link_libraries(
		comp_vk_native
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			aux_vk
			aux_util
			aux_os
			aux_math
			xrt-external-stb
		)

	target_include_directories(comp_vk_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	if(WIN32)
		target_link_libraries(comp_vk_native PRIVATE comp_win32_window)
	endif()

	if(APPLE)
		target_sources(comp_vk_native PRIVATE
			comp_vk_native_window_macos.m
			comp_vk_native_window_macos.h)
		target_link_libraries(comp_vk_native PRIVATE
			"-framework Cocoa" "-framework QuartzCore" "-framework IOSurface")
	endif()

	# Qwerty include path for runtime-side 2D/3D toggle polling
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_vk_native PRIVATE drv_qwerty_includes)
		target_compile_definitions(comp_vk_native PRIVATE XRT_BUILD_DRIVER_QWERTY)
	endif()
endif()
