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

# --- Installed-tree manifest + install rules (#902 packaging) ----------------
#
# Without these the layer is BUILT but never SHIPPED, and the shared-queue
# repaint tier is unreachable on every single-graphics-queue GPU (Intel iGPUs,
# AMD) — the compositor logs "repaint disabled" and degrades silently, so
# nothing announces the absence. Verified 2026-08-11: no install rule, no
# installer entry, no ExplicitLayers key anywhere in the tree.
#
# The installed manifest uses a path RELATIVE to the manifest file (the Vulkan
# loader resolves library_path that way), so the DLL and its .json ship side by
# side in bin/ and the pair relocates with the install directory. The dev-tree
# manifest above keeps its absolute path — run scripts point VK_LAYER_PATH at
# the build dir and never move.
# The prefix is load-bearing, and on Windows it must be a BACKSLASH.
# The loader only treats library_path as a path — and only then joins it to the
# manifest's own directory — when it contains the platform DIRECTORY_SYMBOL
# ('\' on Windows, '/' on POSIX). Anything else (a bare filename, or "./name"
# on Windows) is handed to LoadLibraryEx verbatim, and LoadLibraryEx rejects
# every non-absolute path when called with LOAD_LIBRARY_SEARCH_* flags:
# error 87, ERROR_INVALID_PARAMETER. The loader then reports that layer-library
# open failure to the app as VK_ERROR_OUT_OF_HOST_MEMORY out of
# vkCreateInstance — naming neither the layer nor the path. That is what broke
# the Vulkan media-player demo. Measured with VK_LOADER_DEBUG=all against a
# real install: ".\name.dll" logs `Insert instance layer ... (C:\Program
# Files\DisplayXR\Runtime\VkLayer_DXR_queue_lock.dll)`; the bare name logs
# `Failed to open dynamic library "VkLayer_DXR_queue_lock.dll" with error 87`.
# (The NSIS installer sidesteps all of this by writing an absolute $INSTDIR
# path, which LoadLibraryEx accepts regardless of separator style.)
if(WIN32)
	# Two backslashes in the generated JSON = one after JSON unescaping.
	set(DXR_QL_LIBRARY_PATH ".\\\\VkLayer_DXR_queue_lock.dll")
else()
	set(DXR_QL_LIBRARY_PATH "./libVkLayer_DXR_queue_lock.so")
endif()
configure_file(
	VkLayer_DXR_queue_lock.json.in
	${CMAKE_CURRENT_BINARY_DIR}/installed/VkLayer_DXR_queue_lock.json @ONLY
	)

install(TARGETS VkLayer_DXR_queue_lock LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
                                       RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/installed/VkLayer_DXR_queue_lock.json
        DESTINATION ${CMAKE_INSTALL_BINDIR})
