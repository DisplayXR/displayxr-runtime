# Copyright 2024-2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# Native D3D11 compositor library
#

if(WIN32 AND XRT_HAVE_D3D11)
	# The cross-adapter atlas bridge is its own static library and lives in
	# ../xbridge — see that directory's CMakeLists.txt for why.

	# Multi-screen M6: the per-screen segment manager and the output-device
	# composite unit it blits flat 2D through. Their own static library so the
	# D3D11 service compositor (../d3d11_service) can segment IPC clients'
	# windows without linking the whole in-process compositor — the same reason
	# comp_xbridge is split out.
	add_library(
		comp_d3d11_segs STATIC
		../d3d_shared/comp_masked_composite_shaders.h
		comp_d3d11_outcomp.cpp
		comp_d3d11_outcomp.h
		comp_d3d11_segments.cpp
		comp_d3d11_segments.h
		d3d11_layer_shaders.h
		)
	target_link_libraries(
		comp_d3d11_segs
		PUBLIC xrt-interfaces
		PRIVATE comp_util aux_util aux_os aux_d3d d3d11 d3dcompiler
		)
	target_include_directories(comp_d3d11_segs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	add_library(
		comp_d3d11 STATIC
		../d3d_shared/comp_equirect2_shaders.h
		../d3d_shared/comp_masked_composite_shaders.h
		comp_d3d11_compositor.cpp
		comp_d3d11_compositor.h
		comp_d3d11_compositor_internals.h
		comp_d3d11_renderer.cpp
		comp_d3d11_renderer.h
		comp_d3d11_state_guard.h
		comp_d3d11_swapchain.cpp
		comp_d3d11_swapchain.h
		comp_d3d11_target.cpp
		comp_d3d11_target.h
		d3d11_layer_shaders.h
		)

	target_link_libraries(
		comp_d3d11
		PUBLIC xrt-interfaces
		PRIVATE
			comp_util
			comp_win32_window
			aux_util
			aux_os
			aux_math
			# #918: aux_d3d supplies the scanout-adapter resolver; d3d12 is the
			# cross-adapter atlas bridge (D3D11 has no cross-adapter texture
			# path at all — see docs/investigations/hybrid-igpu-weave.md).
			aux_d3d
			comp_xbridge
			comp_d3d11_segs
			xrt-external-stb
			d3d11
			d3d12
			dxgi
			d3dcompiler
			dcomp
		)

	target_include_directories(comp_d3d11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

	# Qwerty include path for runtime-side 2D/3D toggle polling
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_d3d11 PRIVATE drv_qwerty_includes)
		target_compile_definitions(comp_d3d11 PRIVATE XRT_BUILD_DRIVER_QWERTY)
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
