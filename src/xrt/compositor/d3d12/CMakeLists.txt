# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# Native D3D12 compositor library
#

if(WIN32 AND XRT_HAVE_D3D12)
	add_library(
		comp_d3d12 STATIC
		../d3d_shared/comp_masked_composite_shaders.h
		comp_d3d12_compositor.cpp
		comp_d3d12_compositor.h
		comp_d3d12_outcomp.cpp
		comp_d3d12_outcomp.h
		comp_d3d12_renderer.cpp
		comp_d3d12_renderer.h
		comp_d3d12_swapchain.cpp
		comp_d3d12_swapchain.h
		comp_d3d12_target.cpp
		comp_d3d12_target.h
		)

	target_link_libraries(
		comp_d3d12
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			comp_win32_window
			aux_util
			aux_os
			aux_math
			# #918 D12-3: aux_d3d supplies the scanout-adapter resolver, and
			# comp_xbridge is the cross-adapter atlas transport. Same two
			# dependencies the D3D11 compositor takes, for the same reason —
			# the bridge is a standalone unit precisely so each compositor can
			# link it without dragging the other one in.
			aux_d3d
			comp_xbridge
			xrt-external-stb
			d3d12
			dxgi
			d3dcompiler
			dcomp
		)

	target_include_directories(comp_d3d12 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	# Qwerty include path for runtime-side 2D/3D toggle polling
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_d3d12 PRIVATE drv_qwerty_includes)
		target_compile_definitions(comp_d3d12 PRIVATE XRT_BUILD_DRIVER_QWERTY)
	endif()
endif()
