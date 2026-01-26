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
	# Include layer accumulator directly (it has no Vulkan dependencies)
	../util/comp_layer_accum.c
	../util/comp_layer_accum.h
	)

target_link_libraries(
	comp_d3d11_service
	PRIVATE
		xrt-interfaces
		aux_util
		aux_os
		aux_math
		comp_multi
		WIL::WIL
		${D3D11_LIBRARY}
		${DXGI_LIBRARY}
		d3dcompiler
	)

target_include_directories(comp_d3d11_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(XRT_HAVE_LEIA_SR)
	target_link_libraries(comp_d3d11_service PRIVATE drv_leiasr)
endif()
