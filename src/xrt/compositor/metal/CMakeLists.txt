# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# Native Metal compositor library
#

if(APPLE AND XRT_HAVE_METAL)
	add_library(
		comp_metal STATIC
		comp_metal_compositor.m
		comp_metal_compositor.h
		)

	target_link_libraries(
		comp_metal
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			aux_util
			aux_os
			aux_math
			# HEADERS ONLY (#1581). The Metal compositor calls no Vulkan and
			# links no Vulkan symbols, but util/comp_render_helpers.h — the
			# home of the shared layer helpers, including the N-view
			# eye-visibility rule the quad pass needs — includes
			# render/render_interface.h, which includes vk_helpers.h. This is
			# how the helper header reaches the include path. comp_metal
			# already hard-depends on XRT_HAVE_VULKAN via comp_util, which is
			# itself Vulkan-gated, so this adds no new configuration.
			aux_vk
			xrt-external-stb
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
