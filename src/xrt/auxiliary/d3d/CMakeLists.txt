# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(
	aux_d3d STATIC
	d3d_dxgi_formats.h
	d3d_dxgi_helpers.cpp
	d3d_dxgi_helpers.hpp
	d3d_render_adapter.cpp
	d3d_render_adapter.h
	d3d_render_adapter.hpp
	d3d_scanout_helpers.cpp
	d3d_scanout_helpers.h
	d3d_scanout_helpers.hpp
	d3d_weave_placement.cpp
	d3d_weave_placement.h
	)
# user32: MonitorFromPoint / GetMonitorInfoW / QueryDisplayConfig, used by the
# scanout-adapter resolver. Linked explicitly rather than leaning on MSVC's
# default library set.
# aux_util: u_logging (long-standing, previously resolved only at final link)
# and, since #1252, u_setting for the DXR_D3D_FORCE_GPU settings chain. No
# cycle — aux_util does not link aux_d3d.
target_link_libraries(aux_d3d PUBLIC aux-includes xrt-interfaces aux_util ${DXGI_LIBRARY} WIL::WIL user32)

# needed for format includes
target_include_directories(aux_d3d PUBLIC ${Vulkan_INCLUDE_DIR})

if(XRT_HAVE_D3D11)
	target_sources(
		aux_d3d
		PRIVATE
			d3d_d3d11_allocator.cpp
			d3d_d3d11_allocator.h
			d3d_d3d11_allocator.hpp
			d3d_d3d11_bits.h
			d3d_d3d11_fence.cpp
			d3d_d3d11_fence.hpp
			d3d_d3d11_helpers.cpp
			d3d_d3d11_helpers.hpp
		)
	target_link_libraries(aux_d3d PUBLIC ${D3D11_LIBRARY})
endif()

if(XRT_HAVE_D3D12)
	target_sources(
		aux_d3d
		PRIVATE
			d3d_d3d12_bits.h
			d3d_d3d12_allocator.cpp
			d3d_d3d12_allocator.h
			d3d_d3d12_allocator.hpp
			d3d_d3d12_fence.cpp
			d3d_d3d12_fence.hpp
			d3d_d3d12_helpers.cpp
			d3d_d3d12_helpers.hpp
		)
	target_link_libraries(aux_d3d PUBLIC ${D3D12_LIBRARY})
endif()
