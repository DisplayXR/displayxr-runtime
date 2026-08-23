# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# Native Vulkan compositor library
#

if((WIN32 OR APPLE OR ANDROID OR (XRT_HAVE_LINUX AND XRT_HAVE_XCB)) AND XRT_HAVE_VULKAN)
	# XR_DXR_display_zones alpha-over draw path (ADR-027): zone layers draw
	# through a blend pipeline instead of vkCmdBlitImage.
	spirv_shaders(
		VK_NATIVE_SHADER_HEADERS
		SPIRV_VERSION
		1.0
		SOURCES
		shaders/zone_blit.vert
		shaders/zone_blit.frag
		)

	add_library(
		comp_vk_native STATIC
		${VK_NATIVE_SHADER_HEADERS}
		comp_vk_native_compositor.c
		comp_vk_native_compositor.h
		comp_vk_native_renderer.c
		comp_vk_native_renderer.h
		comp_vk_native_swapchain.c
		comp_vk_native_swapchain.h
		comp_vk_native_target.cpp
		comp_vk_native_target.h
		)

	# Generated SPIR-V headers land under the binary dir mirroring shaders/.
	target_include_directories(comp_vk_native PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

	if(WIN32)
		# DComp + WGL_NV_DX_interop2 dependencies for the VK->D3D11 transparent
		# present bridge. dcomp.h is C++-only, hence the .cpp rename above.
		target_link_libraries(comp_vk_native PRIVATE d3d11 dxgi dcomp)
		# #918 Phase 3: the canonical `weave placement:` line. aux_d3d is LINKED
		# (the formatter lives there); comp_xbridge is INCLUDED ONLY — this
		# compositor has no split, so all it takes from the gate is the reason
		# tokens, and linking the transport would drag D3D12 in for a string.
		target_link_libraries(comp_vk_native PRIVATE aux_d3d)
		target_include_directories(
			comp_vk_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../xbridge
			)
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

	if(ANDROID)
		# Surface-lifecycle re-sync reads the live ANativeWindow published by the
		# SurfaceView callbacks via android_globals_* (#507).
		target_link_libraries(comp_vk_native PRIVATE aux_android)
	endif()

	if(XRT_HAVE_LINUX AND XRT_HAVE_XCB)
		# Linux X11 present path (Phase 1): self-created XCB window + VK_KHR_xcb_surface.
		# XCB_{LIBRARIES,INCLUDE_DIRS} come from the top-level pkg_check_modules(XCB ...).
		target_sources(comp_vk_native PRIVATE
			comp_vk_native_window_xcb.c
			comp_vk_native_window_xcb.h)
		target_include_directories(comp_vk_native PRIVATE ${XCB_INCLUDE_DIRS})
		target_link_libraries(comp_vk_native PRIVATE ${XCB_LIBRARIES})
	endif()

	if(XRT_HAVE_XLIB_XRANDR)
		# Direct-scanout present path (ST-5539): steal a RandR output as a
		# VkDisplayKHR and present to it directly, bypassing Xorg/compositor.
		# Only this TU calls Xlib/Xrandr (XOpenDisplay, XRR*) — the aux VK
		# display helpers are pure Vulkan. XLIB_XRANDR_{LIBRARIES,INCLUDE_DIRS}
		# come from the top-level pkg_check_modules(XLIB_XRANDR x11 xrandr).
		target_sources(comp_vk_native PRIVATE
			comp_vk_native_window_direct.c
			comp_vk_native_window_direct.h)
		target_include_directories(comp_vk_native PRIVATE ${XLIB_XRANDR_INCLUDE_DIRS})
		target_link_libraries(comp_vk_native PRIVATE ${XLIB_XRANDR_LIBRARIES})
	endif()

	if(XRT_HAVE_LINUX AND XRT_HAVE_WAYLAND)
		# Linux Wayland present path (runtime#757 WS3b): app-provided wl_surface +
		# VK_KHR_wayland_surface. No self-created window / xdg-shell — the app owns
		# the surface lifecycle. WAYLAND_{LIBRARIES,INCLUDE_DIRS} come from the
		# top-level pkg_check_modules(WAYLAND wayland-client).
		#
		# NOTE: VK_USE_PLATFORM_WAYLAND_KHR and XRT_HAVE_WAYLAND deliberately do
		# NOT appear as target defines here — they flow globally from
		# xrt_config_vulkan.h / xrt_config_have.h. A target-local
		# VK_USE_PLATFORM_* skews the struct vk_bundle layout against aux_vk
		# (conditional members) — see the platform-flags block in the top
		# CMakeLists.txt for the full failure mode.
		target_include_directories(comp_vk_native PRIVATE ${WAYLAND_INCLUDE_DIRS})
		target_link_libraries(comp_vk_native PRIVATE ${WAYLAND_LIBRARIES})

		if(XRT_HAVE_DBUS)
			# Wayland window-geometry provider (#817): Wayland never exposes a
			# surface's absolute position, so windowed weaving reads it from
			# the compositor-published session-bus service (GNOME Shell
			# extension in contrib/gnome-shell/). Optional — without it the
			# Wayland path stays display-scoped, exactly as before.
			target_sources(comp_vk_native PRIVATE
				comp_vk_native_wl_geom.c
				comp_vk_native_wl_geom.h)
			target_include_directories(comp_vk_native SYSTEM PRIVATE ${DBUS_INCLUDE_DIRS})
			target_link_libraries(comp_vk_native PRIVATE ${DBUS_LIBRARIES})
		endif()
	endif()

	# Qwerty include path for runtime-side 2D/3D toggle polling
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_vk_native PRIVATE drv_qwerty_includes)
		target_compile_definitions(comp_vk_native PRIVATE XRT_BUILD_DRIVER_QWERTY)
	endif()
endif()
