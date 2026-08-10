# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# VK_LAYER_DXR_queue_lock (#902) — per-queue submit serialization so the #868
# late-weave repaint can share the app's VkQueue on single-graphics-queue GPUs.
# Deliberately dependency-free: Vulkan headers only (a layer must NOT link the
# loader — recursion), no xrt/aux libs.

add_library(VkLayer_DXR_queue_lock MODULE queue_lock_layer.c)

# Windows exports come from the .def (no dllexport in code — vk_layer.h's plain
# declarations clash with dllexport redefinitions, MSVC C2375).
if(WIN32)
	target_sources(VkLayer_DXR_queue_lock PRIVATE VkLayer_DXR_queue_lock.def)
endif()

target_include_directories(VkLayer_DXR_queue_lock PRIVATE ${Vulkan_INCLUDE_DIR})

if(NOT WIN32)
	find_package(Threads REQUIRED)
	target_link_libraries(VkLayer_DXR_queue_lock PRIVATE Threads::Threads)
endif()

# The per-config properties matter: multi-config generators (VS; Ninja
# Multi-Config) append a $<CONFIG> subdir to the plain property, landing the
# DLL in vk_layer/Release/ while the manifest below points at vk_layer/ — the
# loader then SILENTLY skips the layer (found in the #902 Windows validation).
set_target_properties(
	VkLayer_DXR_queue_lock
	PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		   LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_BINARY_DIR}
		   LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_BINARY_DIR}
		   LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_CURRENT_BINARY_DIR}
		   LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_CURRENT_BINARY_DIR}
		   C_VISIBILITY_PRESET hidden
	)

# Belt and braces vs the ELF interposition trap (see the TRAP note in
# queue_lock_layer.c): libvulkan also exports vkGetInstanceProcAddr, and a
# non-symbolic internal reference would bind to the loader's copy.
if(NOT WIN32 AND NOT APPLE)
	target_link_options(VkLayer_DXR_queue_lock PRIVATE "LINKER:-Bsymbolic")
endif()

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
