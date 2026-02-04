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
		comp_d3d11_window.cpp
		comp_d3d11_window.h
		)

	target_link_libraries(
		comp_d3d11
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			aux_util
			aux_os
			aux_math
			d3d11
			dxgi
			d3dcompiler
		)

	target_include_directories(comp_d3d11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	# Link to qwerty driver for Win32 input from main window
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_d3d11 PRIVATE drv_qwerty drv_qwerty_includes)
	endif()

	# Link to SR SDK D3D11 weaver if available
	# drv_leiasr exports XRT_HAVE_LEIA_SR_D3D11 as PUBLIC when D3D11 weaver is available
	if(XRT_HAVE_LEIA_SR)
		target_link_libraries(comp_d3d11 PRIVATE drv_leiasr)
	endif()

	# Debug GUI support (XRT_DEBUG_GUI=1 to enable)
	if(XRT_FEATURE_DEBUG_GUI)
		target_sources(
			comp_d3d11
			PRIVATE
				comp_d3d11_debug.cpp
				comp_d3d11_debug.h
			)
		target_link_libraries(comp_d3d11 PRIVATE aux_util_debug_gui)
	endif()

	# Install HLSL shaders (for reference, actual shaders are compiled at runtime)
	# file(GLOB HLSL_SHADERS shaders/*.hlsl)
	# install(FILES ${HLSL_SHADERS} DESTINATION share/monado/shaders/d3d11)
endif()
