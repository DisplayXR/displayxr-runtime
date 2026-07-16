# Copyright 2019-2024, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

###
# Client library
#

add_library(comp_client STATIC)

target_link_libraries(
	comp_client
	PUBLIC xrt-interfaces
	PRIVATE aux_util
	)
target_include_directories(comp_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(XRT_HAVE_VULKAN)
	target_sources(
		comp_client PRIVATE client/comp_vk_client.c client/comp_vk_client.h
				    client/comp_vk_glue.c
		)
	target_link_libraries(comp_client PRIVATE aux_vk)
endif()

if(XRT_HAVE_OPENGL OR XRT_HAVE_OPENGLES)
	target_sources(
		comp_client
		PRIVATE client/comp_gl_client.c client/comp_gl_client.h
			client/comp_gl_memobj_swapchain.c client/comp_gl_memobj_swapchain.h
		)
	target_link_libraries(comp_client PRIVATE aux_ogl)
endif()

if(XRT_HAVE_OPENGL)
	target_sources(comp_client PRIVATE client/comp_gl_glue.c)
endif()

if(XRT_HAVE_OPENGLES)
	target_sources(comp_client PRIVATE client/comp_gles_glue.c)
endif()

if(XRT_HAVE_EGL)
	target_sources(
		comp_client
		PRIVATE client/comp_gl_eglimage_swapchain.c client/comp_gl_eglimage_swapchain.h
			client/comp_egl_client.c client/comp_egl_client.h
		)
endif()

if(XRT_HAVE_OPENGL_GLX AND XRT_HAVE_XLIB)
	target_sources(
		comp_client PRIVATE client/comp_gl_xlib_client.c client/comp_gl_xlib_client.h
				    client/comp_gl_xlib_glue.c
		)
	target_link_libraries(comp_client PRIVATE OpenGL::GLX)
endif()

if(XRT_HAVE_OPENGL AND WIN32)
	target_sources(
		comp_client PRIVATE client/comp_gl_win32_client.c client/comp_gl_win32_client.h
				    client/comp_gl_win32_glue.c
				    client/comp_gl_d3d11_swapchain.c client/comp_gl_d3d11_swapchain.h
		)
	target_link_libraries(comp_client PRIVATE d3d11 dxgi)
endif()

if(APPLE AND XRT_HAVE_OPENGL AND XRT_HAVE_METAL)
	target_sources(
		comp_client PRIVATE client/comp_gl_macos_client.h client/comp_gl_macos_client.m
				    client/comp_gl_macos_glue.m
		)
	target_link_libraries(comp_client PRIVATE "-framework OpenGL" "-framework IOSurface")
endif()

if(XRT_HAVE_D3D11 OR XRT_HAVE_D3D12)
	target_link_libraries(comp_client PRIVATE aux_d3d)
	target_sources(comp_client PRIVATE client/comp_d3d_common.cpp client/comp_d3d_common.hpp)
	# Render-API-agnostic transparent DirectComposition present for forced-IPC
	# transparent clients (ADR-029). The client must own the present because a
	# process cannot create a DComp target on another process's HWND; this helper
	# imports the service's shared output texture + fence and presents on the app's
	# window. Shared by the D3D11, D3D12 and GL (win32) client compositors, so it
	# (and the d3d11/dxgi/dcomp libs it needs) lives in the shared D3D block.
	target_sources(
		comp_client PRIVATE client/comp_d3d_transparent_present.cpp
				    client/comp_d3d_transparent_present.h
		)
	target_link_libraries(comp_client PRIVATE d3d11 dxgi dcomp)
endif()

if(XRT_HAVE_D3D11)
	target_sources(
		comp_client PRIVATE client/comp_d3d11_client.cpp client/comp_d3d11_client.h
				    client/comp_d3d11_glue.c
		)
endif()

if(XRT_HAVE_D3D12)
	target_sources(
		comp_client PRIVATE client/comp_d3d12_client.cpp client/comp_d3d12_client.h
				    client/comp_d3d12_glue.c
		)
endif()

##
# Render library
#

if(XRT_HAVE_VULKAN)
	set(SHADERS
	    shaders/blit.comp
	    shaders/clear.comp
	    shaders/distortion.comp
	    shaders/layer.comp
	    shaders/mesh.frag
	    shaders/mesh.vert
	    shaders/layer_cylinder.frag
	    shaders/layer_cylinder.vert
	    shaders/layer_equirect2.frag
	    shaders/layer_equirect2.vert
	    shaders/layer_projection.vert
	    shaders/layer_quad.vert
	    shaders/layer_shared.frag
		)

	spirv_shaders(
		SHADER_HEADERS
		SPIRV_VERSION
		1.0 # Currently targeting Vulkan 1.0
		SOURCES
		${SHADERS}
		)

	add_library(
		comp_render STATIC
		${SHADER_HEADERS}
		render/render_buffer.c
		render/render_compute.c
		render/render_distortion.c
		render/render_gfx.c
		render/render_interface.h
		render/render_resources.c
		render/render_shaders.c
		render/render_sub_alloc.c
		render/render_util.c
		)
	# The aux_vk library needs to be public to include Vulkan.
	target_link_libraries(
		comp_render
		PUBLIC xrt-interfaces aux_vk
		PRIVATE aux_util aux_os
		)
	# So it can be used without any other compositor libraries.
	target_include_directories(comp_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
	# Shaders - marked SYSTEM so we get no warnings
	target_include_directories(comp_render SYSTEM PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

	add_subdirectory(shaders)
endif()

##
# Util library
#

if(XRT_HAVE_VULKAN)
	add_library(
		comp_util STATIC
		util/comp_base.h
		util/comp_base.c
		util/comp_high_level_render.c
		util/comp_high_level_render.h
		util/comp_high_level_scratch.c
		util/comp_high_level_scratch.h
		util/comp_layer_accum.h
		util/comp_layer_accum.c
		util/comp_render.h
		util/comp_render_cs.c
		util/comp_render_gfx.c
		util/comp_render_helpers.h
		util/comp_scratch.c
		util/comp_scratch.h
		util/comp_semaphore.h
		util/comp_semaphore.c
		util/comp_swapchain.h
		util/comp_swapchain.c
		util/comp_sync.h
		util/comp_sync.c
		util/comp_target_service.h
		util/comp_vulkan.h
		util/comp_vulkan.c
		)
	target_link_libraries(
		comp_util
		PUBLIC xrt-interfaces
		PRIVATE aux_util aux_os aux_vk comp_render
		)
	target_include_directories(comp_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()

##
# Main library — REMOVED (issue #25: lightweight runtime no longer needs
# the Vulkan server compositor).  Shared interface headers (comp_target.h,
# comp_target_swapchain.h/c, comp_frame.h) remain in main/ for comp_multi
# and null compositor use.

###
# Shared macOS window utility (MoltenVK CAMetalLayer NSWindow + the per-session
# comp_target_swapchain it drives).
#
# Parallels comp_win32_window below: both comp_null (per-session present path,
# #48) and comp_multi (shared-surface visibility toggle via
# comp_window_macos_set_visible, #61) depend on this helper. A --service
# openxr_displayxr.dylib links comp_multi WITHOUT comp_null, so the helper can't
# live only inside comp_null or that link fails with an undefined
# comp_window_macos_set_visible. Build it once here so every consumer links it.
# Defined before add_subdirectory(null) so comp_null can link it.
if(APPLE)
	add_library(
		comp_macos_window STATIC
		main/comp_window_macos.m
		main/comp_window_macos.h
		main/comp_target_swapchain.c
		)
	target_link_libraries(
		comp_macos_window
		PUBLIC xrt-interfaces
		PRIVATE aux_util
			aux_os
			aux_vk
			"-framework Cocoa"
			"-framework QuartzCore"
			"-framework Metal"
		)
	target_include_directories(comp_macos_window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()

###
# Null compositor
#

if(XRT_MODULE_COMPOSITOR_NULL)
	add_subdirectory(null)
endif()

###
# Shared Win32 window utility (pure Win32, no D3D11 dependency)
#
if(WIN32)
	add_library(
		comp_win32_window STATIC
		d3d11/comp_d3d11_window.cpp
		d3d11/comp_d3d11_window.h
		)
	target_link_libraries(
		comp_win32_window
		PUBLIC xrt-interfaces
		PRIVATE aux_util aux_os
		)
	target_include_directories(comp_win32_window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/d3d11)
	if(XRT_BUILD_DRIVER_QWERTY)
		target_link_libraries(comp_win32_window PRIVATE drv_qwerty drv_qwerty_includes)
		target_compile_definitions(comp_win32_window PRIVATE XRT_BUILD_DRIVER_QWERTY)
	endif()
endif()

###
# Multi client compositor library
#

add_library(
	comp_multi STATIC multi/comp_multi_compositor.c multi/comp_multi_interface.h
			  multi/comp_multi_private.h multi/comp_multi_system.c
			  multi/comp_multi_workspace.c multi/comp_multi_workspace.h
			  multi/comp_multi_content_blend.c multi/comp_multi_content_blend.h
	)
target_link_libraries(
	comp_multi
	PUBLIC xrt-interfaces
	PRIVATE aux_util aux_os aux_vk
	)
target_include_directories(comp_multi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# comp_render provides render_shaders_load/fini used by window-space layer compositing
target_link_libraries(comp_multi PRIVATE comp_render)

# Qwerty include path for runtime-side 2D/3D toggle polling
if(XRT_BUILD_DRIVER_QWERTY)
	target_link_libraries(comp_multi PRIVATE drv_includes)
	target_compile_definitions(comp_multi PRIVATE XRT_BUILD_DRIVER_QWERTY)
endif()

# Link comp_multi to shared Win32 window for self-owned window creation
if(WIN32)
	target_link_libraries(comp_multi PRIVATE comp_win32_window)
endif()

# Link comp_multi to the shared macOS window helper for shared-surface
# visibility toggling (comp_window_macos_set_visible, #61). Without this a
# --service openxr_displayxr.dylib (which links comp_multi but not comp_null)
# fails to link with an undefined comp_window_macos_set_visible.
if(APPLE)
	target_link_libraries(comp_multi PRIVATE comp_macos_window)
	# XR_DXR_weave present-owner service (#759): IOSurface in/out + the
	# Vulkan weave engine.
	target_sources(comp_multi PRIVATE multi/comp_multi_weave_macos.c)
	target_link_libraries(comp_multi PRIVATE "-framework IOSurface" "-framework CoreFoundation")
endif()

###
# Mock native compositor
add_library(comp_mock STATIC mock/mock_compositor.cpp mock/mock_compositor.h)
target_include_directories(comp_mock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
	comp_mock
	PUBLIC xrt-interfaces
	PRIVATE aux_util
	)

###
# Native D3D11 compositor (bypasses Vulkan entirely)
#
if(WIN32 AND XRT_HAVE_D3D11)
	add_subdirectory(d3d11)
endif()

###
# Native D3D12 compositor (bypasses Vulkan entirely)
#
if(WIN32 AND XRT_HAVE_D3D12)
	add_subdirectory(d3d12)
endif()

###
# Native Metal compositor (bypasses Vulkan entirely)
#
if(APPLE AND XRT_HAVE_METAL)
	add_subdirectory(metal)
endif()

###
# Native Vulkan compositor (direct submit, no multi-compositor)
#
if((WIN32 OR APPLE OR ANDROID OR (XRT_HAVE_LINUX AND XRT_HAVE_XCB)) AND XRT_HAVE_VULKAN)
	add_subdirectory(vk_native)
endif()

###
# Native OpenGL compositor (bypasses Vulkan entirely)
#
# Win32 + macOS only: those are the only platforms that link it (st_oxr's
# XRT_HAVE_GL_NATIVE_COMPOSITOR arms). Linux is Vulkan-only — no GL window
# backend exists (#709) — and Android uses GLES (XRT_HAVE_OPENGL is never
# set there).
if((WIN32 OR APPLE) AND XRT_HAVE_OPENGL)
	add_subdirectory(gl)
endif()

###
# D3D11 Service compositor (for hybrid mode)
#
if(WIN32 AND XRT_FEATURE_HYBRID_MODE)
	add_subdirectory(d3d11_service)
endif()
