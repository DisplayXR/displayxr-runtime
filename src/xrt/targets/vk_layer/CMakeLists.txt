# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# VK_LAYER_DXR_queue_lock (#902) — per-queue submit serialization so the #868
# late-weave repaint can share the app's VkQueue on single-graphics-queue GPUs.
# Deliberately dependency-free: Vulkan headers only (a layer must NOT link the
# loader — recursion), no xrt/aux libs.

add_library(VkLayer_DXR_queue_lock MODULE queue_lock_layer.c)

target_include_directories(VkLayer_DXR_queue_lock PRIVATE ${Vulkan_INCLUDE_DIR})

if(NOT WIN32)
	find_package(Threads REQUIRED)
	target_link_libraries(VkLayer_DXR_queue_lock PRIVATE Threads::Threads)
endif()

set_target_properties(
	VkLayer_DXR_queue_lock PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
					  C_VISIBILITY_PRESET hidden
	)

# Dev-tree manifest with an absolute library_path — run scripts point
# VK_LAYER_PATH here. (Installed builds get a relative-path manifest via the
# packaging step instead; Windows additionally needs the ExplicitLayers
# registry key — VK_LAYER_PATH is ignored for elevated processes.)
if(WIN32)
	set(DXR_QL_LIBRARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_DXR_queue_lock.dll")
else()
	set(DXR_QL_LIBRARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/libVkLayer_DXR_queue_lock.so")
endif()
configure_file(
	VkLayer_DXR_queue_lock.json.in ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_DXR_queue_lock.json @ONLY
	)
