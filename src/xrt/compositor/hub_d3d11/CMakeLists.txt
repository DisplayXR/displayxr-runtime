# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# The D3D11 hub (#2317): the Vulkan-free D3D11 "fill arm" that weaves with the
# D3D11 display processor and presents, for whichever producer hands it a
# deposit (comp_hub_d3d11_handoff.h). Born as comp_vk_native's VK-1 output-device
# split (#918 / #1178) and extracted here by #2317 P1a, so the D3D12 reroute
# (#1264) and the D3D11 shim (#2317 P1b) link it without dragging in — or
# requiring — Vulkan.
#
# Built wherever a consumer exists. Off Windows (or without D3D11) the TU
# compiles to its honest-NO stubs, which the Vulkan compositor still calls on
# macOS / Linux / Android.
#
add_library(
	comp_hub_d3d11 STATIC
	comp_hub_d3d11.cpp
	comp_hub_d3d11.h
	comp_hub_d3d11_handoff.h
	)

target_link_libraries(
	comp_hub_d3d11
	PUBLIC xrt-interfaces
	PRIVATE comp_util aux_util aux_os aux_math
	)

target_include_directories(comp_hub_d3d11 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(WIN32 AND XRT_HAVE_D3D11)
	# aux_d3d carries the canonical `weave placement:` formatter and the scanout
	# adapter resolver; comp_xbridge is the cross-adapter transport (its D3D11
	# ends are exactly what a deposit hands over); comp_d3d11 provides the
	# scanout-adapter DXGI swapchain (comp_d3d11_target), reused verbatim; and
	# comp_d3d11_segs segments a spanning window on the scanout adapter
	# (ADR-047 Amendment 1).
	target_link_libraries(
		comp_hub_d3d11
		PRIVATE
			aux_d3d
			comp_xbridge
			comp_d3d11
			comp_d3d11_segs
			d3d11
			dxgi
		)
endif()
