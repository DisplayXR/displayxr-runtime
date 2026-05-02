# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# D3D11 Service Compositor
#
# This compositor runs in service mode (IPC server) with its own D3D11 device,
# independent of client applications. It imports swapchains from clients via
# DXGI shared handles and uses KeyedMutex for synchronization.
#

# Embed doc/displayxr_white.png as a byte array so the compositor can draw the
# logo on the empty-state screen without shipping the PNG alongside the binary.
set(DISPLAYXR_LOGO_SRC "${CMAKE_SOURCE_DIR}/doc/displayxr_white.png")
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

add_library(
	comp_d3d11_service STATIC
	comp_d3d11_service.cpp
	comp_d3d11_service.h
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


