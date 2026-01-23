# Copyright 2024-2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

#
# Native D3D11 compositor library
#

if(WIN32 AND XRT_HAVE_D3D11)
	add_library(
		comp_d3d11 STATIC
		comp_d3d11_compositor.cpp
		comp_d3d11_compositor.h
		comp_d3d11_renderer.cpp
		comp_d3d11_renderer.h
		comp_d3d11_swapchain.cpp
		comp_d3d11_swapchain.h
		comp_d3d11_target.cpp
		comp_d3d11_target.h
		)

	target_link_libraries(
		comp_d3d11
		PUBLIC xrt-interfaces
		PRIVATE
			aux_util
			aux_os
			aux_math
			d3d11
			dxgi
			d3dcompiler
		)

	target_include_directories(comp_d3d11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	# Link to SR SDK D3D11 weaver if available
	if(XRT_HAVE_LEIA_SR)
		target_compile_definitions(comp_d3d11 PRIVATE XRT_HAVE_LEIA_SR)
		target_link_libraries(comp_d3d11 PRIVATE drv_leiasr)
	endif()

	# Install HLSL shaders (for reference, actual shaders are compiled at runtime)
	# file(GLOB HLSL_SHADERS shaders/*.hlsl)
	# install(FILES ${HLSL_SHADERS} DESTINATION share/monado/shaders/d3d11)
endif()
