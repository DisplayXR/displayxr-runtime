# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# D3D11 Service Compositor
#
# This compositor runs in service mode (IPC server) with its own D3D11 device,
# independent of client applications. It imports swapchains from clients via
# DXGI shared handles and uses KeyedMutex for synchronization.
#

add_library(
	comp_d3d11_service STATIC
	comp_d3d11_service.cpp
	comp_d3d11_service.h
	d3d11_service_shaders.h
	d3d11_capture.cpp
	d3d11_capture.h
	d3d11_icon_loader.cpp
	d3d11_icon_loader.h
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


