# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# Cross-adapter transport library (#918).
#
# The transport has no compositor in it: it is devices, queues, a cross-adapter
# heap and fences, and its whole dependency set is aux_util + aux_os + the
# Windows graphics libraries. Four consumers want the identical transport (the
# in-process D3D11 compositor, the out-of-process D3D11 service, and the D3D12
# pair behind them), and linking `comp_d3d11` to get at it would drag the entire
# in-process compositor — renderer, swapchains, window, target — into every one
# of them. So the unit stands alone and each compositor links it.
#
# It lives OUTSIDE d3d11/ because its middle was never D3D11: D3D11 has no
# cross-adapter texture path on this stack, so the bridge is D3D12 in the middle
# with whatever the caller has on both ends.
#
if(WIN32 AND XRT_HAVE_D3D11)
	add_library(
		comp_xbridge STATIC
		comp_split_gate.c
		comp_split_gate.h
		comp_xbridge.cpp
		comp_xbridge.h
		comp_xbridge_plane_policy.h
		)

	target_link_libraries(
		comp_xbridge
		PUBLIC xrt-interfaces
		PRIVATE
			aux_util
			aux_os
			d3d11
			d3d12
			dxgi
		)

	# Consumers include the bridge headers by their bare names.
	target_include_directories(comp_xbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()
