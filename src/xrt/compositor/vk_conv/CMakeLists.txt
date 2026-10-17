# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

#
# ADR-053 P5: the conversion core's Vulkan GPU glue + the S5 reference view
# synthesis on Vulkan (the POSIX twins of d3d11_service's d3d11_conv_gpu /
# d3d11_conv_viewsyn). macOS (MoltenVK) and desktop Linux.
#

spirv_shaders(
	VK_CONV_SHADER_HEADERS
	SPIRV_VERSION
	1.0
	SOURCES
	shaders/conv_viewsyn_prep.comp
	shaders/conv_viewsyn_filter.comp
	shaders/conv_viewsyn_guide.comp
	shaders/conv_viewsyn_upsample.comp
	shaders/conv_viewsyn_main.comp
	)

# The stage on its own, so the conversion pipeline's tests link it without the
# glue; the platform-neutral math (compositor/util) rides along, as it does in
# comp_d3d11_conv_viewsyn.
add_library(
	comp_vk_conv_viewsyn STATIC
	${VK_CONV_SHADER_HEADERS}
	vk_conv_viewsyn.c
	vk_conv_viewsyn.h
	../util/u_conv_viewsyn.c
	../util/u_conv_viewsyn.h
	)
target_include_directories(comp_vk_conv_viewsyn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Generated SPIR-V headers land under the binary dir mirroring shaders/.
target_include_directories(comp_vk_conv_viewsyn PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(comp_vk_conv_viewsyn PUBLIC xrt-interfaces aux_vk PRIVATE aux_util)
