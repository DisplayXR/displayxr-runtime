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
		shaders/zone_blit_array.frag
		shaders/zone_blit_soft.frag
		shaders/zone_blit_soft_array.frag
		shaders/equirect2.vert
		shaders/equirect2.frag
		shaders/equirect2_array.frag
		shaders/alpha_probe.comp
		)

	# Multi-screen segment manager (ADR-047 D2), its own library so the
	# desktop-Linux service's XR_DXR_weave engine (comp_multi, #2218 item 6)
	# links it without the whole in-process compositor — the comp_d3d11_segs
	# pattern.
	add_library(comp_vk_segs STATIC comp_vk_native_segments.c comp_vk_native_segments.h)
	target_link_libraries(
		comp_vk_segs
		PUBLIC xrt-interfaces
		PRIVATE comp_util aux_vk aux_util aux_os
		)
	target_include_directories(comp_vk_segs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
	# ...and the macOS service's Vulkan (MoltenVK) weave backend.
	if(TARGET comp_multi AND (XRT_FEATURE_COMP_MULTI_WEAVE_LINUX OR APPLE))
		target_link_libraries(comp_multi PRIVATE comp_vk_segs)
		target_compile_definitions(comp_multi PRIVATE COMP_MULTI_WEAVE_HAVE_SEGMENTS)
	endif()

	add_library(
		comp_vk_native STATIC
		${VK_NATIVE_SHADER_HEADERS}
		comp_vk_native_alpha_probe.c
		comp_vk_native_alpha_probe.h
		comp_vk_native_compositor.c
		comp_vk_native_compositor.h
		comp_vk_native_deposit.cpp
		comp_vk_native_deposit.h
		comp_vk_native_equirect2.h
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
		# #918 VK-1 (#1178): the in-process Vulkan output-device split. aux_d3d
		# carries the canonical `weave placement:` formatter and the scanout
		# adapter resolver; comp_xbridge is now LINKED, not merely included —
		# this compositor HAS a split, and the D3D11 ends of the transport are
		# exactly what the VK-0 deposit hands it. The split itself (the D3D11
		# fill arm) is comp_hub_d3d11 since #2317 P1a, linked below on every
		# platform (stubs off Windows).
		target_link_libraries(comp_vk_native PRIVATE aux_d3d)
		target_include_directories(
			comp_vk_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../xbridge
			)
		if(XRT_HAVE_D3D11)
			# comp_xbridge and comp_d3d11 are themselves built only under this
			# condition, and comp_hub_d3d11 compiles to its honest-NO stubs
			# without it.
			target_link_libraries(comp_vk_native PRIVATE comp_xbridge comp_d3d11)
			# Multi-screen M6 under the split (ADR-047 Amendment 1): the
			# D3D11 segment manager segments a spanning window on the
			# scanout adapter, as the in-process D3D11 compositor does.
			target_link_libraries(comp_vk_native PRIVATE comp_d3d11_segs)
		endif()
	endif()

	target_link_libraries(
		comp_vk_native
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			comp_vk_segs
			# #2317 P1a: the D3D11 fill arm the VK-1 split drives (stubs off
			# Windows / without D3D11).
			comp_hub_d3d11
			aux_vk
			aux_util
			aux_os
			aux_math
			xrt-external-stb
			# XR_DXR_cursor_depth v5: the runtime-drawn cursor's default look.
			comp_cursor_look
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

		# #902 present-feedback feeder: the desktop-Linux source for the vblank
		# grid late weave aims at (X11 Present CompleteNotify, encoded in-tree —
		# no libxcb-present — and native-Wayland wp_presentation, below).
		target_sources(comp_vk_native PRIVATE
			comp_vk_native_present_feedback.c
			comp_vk_native_present_feedback.h)
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

		# #902: wp_presentation glue for the present-feedback feeder, generated
		# from the vendored XML (src/external/wayland-protocols) the same way
		# the service window's protocols are (../CMakeLists.txt). Without
		# wayland-scanner the feeder still builds, X11-only.
		find_program(DXR_WAYLAND_SCANNER NAMES wayland-scanner)
		if(XRT_HAVE_XCB AND DXR_WAYLAND_SCANNER)
			set(_vkpf_xml "${PROJECT_SOURCE_DIR}/src/external/wayland-protocols/presentation-time.xml")
			set(_vkpf_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
			set(_vkpf_hdr "${_vkpf_gen}/presentation-time-client-protocol.h")
			set(_vkpf_src "${_vkpf_gen}/presentation-time-protocol.c")
			add_custom_command(
				OUTPUT "${_vkpf_hdr}"
				COMMAND ${CMAKE_COMMAND} -E make_directory "${_vkpf_gen}"
				COMMAND "${DXR_WAYLAND_SCANNER}" client-header "${_vkpf_xml}" "${_vkpf_hdr}"
				DEPENDS "${_vkpf_xml}"
				VERBATIM
				)
			add_custom_command(
				OUTPUT "${_vkpf_src}"
				COMMAND ${CMAKE_COMMAND} -E make_directory "${_vkpf_gen}"
				COMMAND "${DXR_WAYLAND_SCANNER}" private-code "${_vkpf_xml}" "${_vkpf_src}"
				DEPENDS "${_vkpf_xml}"
				VERBATIM
				)
			target_sources(comp_vk_native PRIVATE "${_vkpf_hdr}" "${_vkpf_src}")
			target_include_directories(comp_vk_native PRIVATE "${_vkpf_gen}")
			# Source-local, not a target define: only the feeder TU reads it, and
			# a target-wide define here is exactly what the vk_bundle note above
			# warns about for VK_USE_PLATFORM_*.
			set_source_files_properties(
				comp_vk_native_present_feedback.c PROPERTIES COMPILE_DEFINITIONS
									     DXR_HAVE_WP_PRESENTATION
				)
		endif()

		if(XRT_HAVE_DBUS)
			# Wayland window-geometry provider (#817): Wayland never exposes a
			# surface's absolute position, so windowed weaving reads it from
			# the compositor-published session-bus service (GNOME Shell
			# extension in contrib/gnome-shell/). Optional — without it the
			# Wayland path stays display-scoped, exactly as before.
			target_sources(comp_vk_native PRIVATE
				comp_vk_native_wl_geom.c
				comp_vk_native_wl_geom.h
				comp_vk_native_wl_move_sync.c
				comp_vk_native_wl_move_sync.h)
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
