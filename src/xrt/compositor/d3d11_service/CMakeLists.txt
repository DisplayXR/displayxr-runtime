# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# D3D11 Service Compositor
#
# This compositor runs in service mode (IPC server) with its own D3D11 device,
# independent of client applications. It imports swapchains from clients via
# DXGI shared handles and uses KeyedMutex for synchronization.
#

# Embed assets/displayxr_white.png as a byte array so the compositor can draw the
# logo on the empty-state screen without shipping the PNG alongside the binary.
set(DISPLAYXR_LOGO_SRC "${CMAKE_SOURCE_DIR}/assets/displayxr_white.png")
set(DISPLAYXR_LOGO_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/displayxr_logo_data.c")
add_custom_command(
	OUTPUT "${DISPLAYXR_LOGO_GENERATED}"
	COMMAND ${CMAKE_COMMAND}
		-DINPUT_FILE=${DISPLAYXR_LOGO_SRC}
		-DOUTPUT_FILE=${DISPLAYXR_LOGO_GENERATED}
		-DSYMBOL=displayxr_white_png
		-P "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary.cmake"
	DEPENDS "${DISPLAYXR_LOGO_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary.cmake"
	VERBATIM
	COMMENT "Embedding ${DISPLAYXR_LOGO_SRC}"
)

# ADR-053 S5: the reference view synthesis (port of the browser's live-dibr.js).
# Its own small library so conv_pipeline and the WARP unit test link it without
# the service compositor; the platform-neutral math (compositor/util) rides along.
add_library(
	comp_d3d11_conv_viewsyn STATIC
	d3d11_conv_viewsyn.cpp
	d3d11_conv_viewsyn.h
	d3d11_conv_viewsyn_shaders.h
	../util/u_conv_viewsyn.c
	../util/u_conv_viewsyn.h
	)
target_include_directories(comp_d3d11_conv_viewsyn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(comp_d3d11_conv_viewsyn PUBLIC xrt-interfaces aux_util PRIVATE ${D3D11_LIBRARY} d3dcompiler)

# #2030: the still-image content check (a private device; GenerateMips down to a
# <= 16x16 block, asynchronous staging readback). Its own library so the WARP
# test (tests_conv_gpu_dedupe) links it without the service.
add_library(comp_d3d11_conv_content STATIC d3d11_conv_content_key.cpp d3d11_conv_content_key.h)
target_include_directories(comp_d3d11_conv_content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(comp_d3d11_conv_content PUBLIC xrt-interfaces aux_util PRIVATE ${D3D11_LIBRARY} ${DXGI_LIBRARY})

# XR_DXR_lift (ADR-042) on the D3D11 service: the lift thread, its device and
# the vendor module's display processor (d3d11_lift.cpp), plus the conversion
# core's D3D11 GPU glue on the service device (d3d11_conv_gpu.cpp, split out in
# ADR-053 P1b). Its own small library so the WARP golden replay
# (tests_lift_dp_golden) links it without the whole service compositor.
add_library(
	comp_d3d11_lift STATIC
	d3d11_conv_gpu.cpp
	d3d11_conv_gpu.h
	d3d11_lift.cpp
	d3d11_lift.h
	d3d11_lift_internal.h
	)
target_include_directories(comp_d3d11_lift PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
	comp_d3d11_lift
	PUBLIC xrt-interfaces aux_util aux_os conv_runtime
	PRIVATE comp_d3d11_conv_viewsyn comp_d3d11_conv_content ${D3D11_LIBRARY} ${DXGI_LIBRARY} d3dcompiler
	)

add_library(
	comp_d3d11_service STATIC
	../d3d_shared/comp_equirect2_shaders.h
	comp_d3d11_service.cpp
	comp_d3d11_service.h
	comp_d3d11_service_conv.h
	d3d11_service_shaders.h
	d3d11_capture.cpp
	d3d11_capture.h
	d3d11_icon_loader.cpp
	d3d11_icon_loader.h
	displayxr_logo_data.h
	"${DISPLAYXR_LOGO_GENERATED}"
	# Include layer accumulator directly (it has no Vulkan dependencies)
	../util/comp_layer_accum.c
	../util/comp_layer_accum.h
	# #1580: the shared per-view camera resolver (no Vulkan deps either)
	../util/comp_layer_view_camera.c
	../util/comp_layer_view_camera.h
	)

target_link_libraries(
	comp_d3d11_service
	PRIVATE
		xrt-interfaces
		comp_win32_window
		aux_util
		aux_os
		aux_math
		aux_d3d  # For D3D11 fence/semaphore support
		WIL::WIL
		${D3D11_LIBRARY}
		${DXGI_LIBRARY}
		d3dcompiler
		d2d1        # Direct2D for font atlas rendering
		dwrite      # DirectWrite for anti-aliased text
		windowsapp  # C++/WinRT runtime for Windows.Graphics.Capture
		dcomp       # DirectComposition for transparent HWND-bound presentation
	)

# #918 Phase 2b: the cross-adapter atlas transport. PR 1/6 extracted it into its
# own static lib precisely so the service can link it without dragging the whole
# in-process compositor (renderer, swapchains, window, target) in. Unconditional:
# this target already hard-requires D3D11 (${D3D11_LIBRARY}, <d3d11_4.h>), so the
# guard that builds the bridge is satisfied wherever this library builds at all.
target_link_libraries(comp_d3d11_service PRIVATE comp_xbridge)

# XR_DXR_lift: the lift thread + the conversion core's D3D11 glue (see above).
target_link_libraries(comp_d3d11_service PRIVATE comp_d3d11_lift)

# Multi-screen M6 on the service path: a direct (non-workspace) IPC client's
# window spanning monitors is woven per screen by the in-process compositor's
# own segment manager (comp_d3d11_segs, split out of comp_d3d11 for exactly
# this). comp_util carries the backend-agnostic segment math (comp_segments.c).
target_link_libraries(comp_d3d11_service PRIVATE comp_d3d11_segs comp_util)

# XR_DXR_cursor_depth v5 over IPC (ADR-046 Phase 4): the service draws IPC
# clients' runtime-drawn cursor with the same default look (the mirrored OS
# cursor, else the classic arrow) and softening knobs as every in-process
# compositor.
target_link_libraries(comp_d3d11_service PRIVATE comp_cursor_look)

target_include_directories(comp_d3d11_service PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_CURRENT_SOURCE_DIR}/../d3d11
	${CMAKE_CURRENT_SOURCE_DIR}/../../ipc          # shared/ipc_protocol.h (launcher app list)
	${CMAKE_CURRENT_SOURCE_DIR}/../../../external/stb  # stb_image.h
)

# Qwerty include path for stereo state polling in HUD overlay
if(XRT_BUILD_DRIVER_QWERTY)
	target_link_libraries(comp_d3d11_service PRIVATE drv_qwerty_includes)
	target_compile_definitions(comp_d3d11_service PRIVATE XRT_BUILD_DRIVER_QWERTY)
endif()


