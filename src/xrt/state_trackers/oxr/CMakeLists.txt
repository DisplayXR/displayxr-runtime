# Copyright 2019-2024, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

###
# Main code
#

add_subdirectory(oxr_bindings)

add_library(
	st_oxr STATIC
	oxr_api_action.c
	oxr_api_funcs.h
	oxr_api_instance.c
	oxr_api_negotiate.c
	oxr_api_session.c
	oxr_api_space.c
	oxr_api_swapchain.c
	oxr_api_system.c
	oxr_api_verify.h
	oxr_api_xdev.c
	oxr_binding.c
	oxr_chain.h
	oxr_dpad.c
	oxr_event.c
	oxr_extension_support.h
	oxr_frame_sync.c
	oxr_frame_sync.h
	oxr_handle_base.c
	oxr_input.c
	oxr_input_transform.c
	oxr_input_transform.h
	oxr_instance.c
	oxr_logger.c
	oxr_logger.h
	oxr_mcp_tools.c
	oxr_mcp_tools.h
	oxr_objects.h
	oxr_path.c
	oxr_pretty_print.c
	oxr_pretty_print.h
	oxr_session.c
	oxr_session_frame_end.c
	oxr_space.c
	oxr_swapchain.c
	oxr_system.c
	oxr_two_call.h
	oxr_verify.c
	oxr_workspace.c
	oxr_workspace_modal_win32.c
	oxr_app_launcher.c
	oxr_xdev.c
	${CMAKE_CURRENT_BINARY_DIR}/oxr_bindings/b_oxr_generated_bindings.c
	${CMAKE_CURRENT_BINARY_DIR}/oxr_bindings/b_oxr_generated_bindings.h
	)

add_dependencies(st_oxr generate_oxr_bindings)
# To enable finding oxr_objects.h in b_oxr_generated_bindings
target_include_directories(st_oxr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(XRT_HAVE_VULKAN)
	target_sources(st_oxr PRIVATE oxr_session_gfx_vk.c oxr_swapchain_vk.c oxr_vulkan.c)
	target_link_libraries(st_oxr PUBLIC aux_vk)
endif()

if(XRT_HAVE_OPENGL OR XRT_HAVE_OPENGLES)
	target_sources(st_oxr PRIVATE oxr_swapchain_gl.c)
	target_link_libraries(st_oxr PUBLIC aux_ogl)
endif()

if(XRT_HAVE_OPENGL_GLX AND XRT_HAVE_XLIB)
	target_sources(st_oxr PRIVATE oxr_session_gfx_gl_xlib.c)
endif()

if(XRT_HAVE_EGL)
	target_sources(st_oxr PRIVATE oxr_session_gfx_egl.c)
endif()

if(XRT_HAVE_D3D11 OR XRT_HAVE_D3D12)
	target_sources(st_oxr PRIVATE oxr_d3d.cpp)
	target_link_libraries(st_oxr PRIVATE aux_d3d)
endif()

if(XRT_HAVE_D3D11)
	target_sources(st_oxr PRIVATE oxr_session_gfx_d3d11.c oxr_swapchain_d3d11.c oxr_d3d11.cpp)

	# D3D11 native compositor support (bypasses Vulkan)
	if(WIN32 AND TARGET comp_d3d11)
		target_sources(st_oxr PRIVATE oxr_session_gfx_d3d11_native.c)
		target_link_libraries(st_oxr PRIVATE comp_d3d11)
		target_compile_definitions(st_oxr PRIVATE XRT_HAVE_D3D11_NATIVE_COMPOSITOR)
	endif()
endif()


if(XRT_HAVE_D3D12)
	target_sources(st_oxr PRIVATE oxr_session_gfx_d3d12.c oxr_swapchain_d3d12.c oxr_d3d12.cpp)
	target_link_libraries(st_oxr PRIVATE aux_d3d)

	# D3D12 native compositor support (bypasses Vulkan)
	if(WIN32 AND TARGET comp_d3d12)
		target_sources(st_oxr PRIVATE oxr_session_gfx_d3d12_native.c oxr_swapchain_d3d12_native.c)
		target_link_libraries(st_oxr PRIVATE comp_d3d12)
		target_compile_definitions(st_oxr PRIVATE XRT_HAVE_D3D12_NATIVE_COMPOSITOR)
	endif()
endif()

if(ANDROID)
	target_sources(st_oxr PRIVATE oxr_session_gfx_gles_android.c)
	target_link_libraries(st_oxr PRIVATE aux_android)
endif()

if(WIN32)
	target_link_libraries(st_oxr PRIVATE kernel32)

	# VK native compositor support (direct Vulkan, no multi-compositor)
	if(XRT_HAVE_VULKAN AND TARGET comp_vk_native)
		target_sources(st_oxr PRIVATE oxr_session_gfx_vk_native.c)
		target_link_libraries(st_oxr PRIVATE comp_vk_native)
		target_compile_definitions(st_oxr PRIVATE XRT_HAVE_VK_NATIVE_COMPOSITOR)
	endif()
endif()

if(APPLE)
	target_sources(st_oxr PRIVATE oxr_macos.m)
	target_link_libraries(st_oxr PRIVATE "-framework Cocoa" "-framework QuartzCore")
	# On macOS, link against both the include targets (for header paths) and the
	# actual driver libraries (for symbols). oxr_macos.m calls qwerty_process_macos(),
	# and the macOS linker resolves symbols in a single left-to-right pass so
	# these must be direct dependencies.
	if(XRT_BUILD_DRIVER_QWERTY AND TARGET drv_qwerty)
		target_link_libraries(st_oxr PRIVATE drv_qwerty drv_qwerty_includes)
	endif()

	# Metal native compositor support (bypasses Vulkan)
	if(XRT_HAVE_METAL AND TARGET comp_metal)
		target_sources(st_oxr PRIVATE oxr_session_gfx_metal_native.c oxr_swapchain_metal.c)
		target_link_libraries(st_oxr PRIVATE comp_metal)
		target_compile_definitions(st_oxr PRIVATE XRT_HAVE_METAL_NATIVE_COMPOSITOR)
		# Vulkan apps on macOS route through Metal native compositor for presentation
		if(XRT_HAVE_VULKAN)
			target_sources(st_oxr PRIVATE oxr_session_gfx_vk_native.c)
		endif()
		# OpenGL apps on macOS: native GL compositor if available, Metal fallback otherwise
		if(XRT_HAVE_OPENGL)
			target_sources(st_oxr PRIVATE oxr_session_gfx_gl_macos.m)
			target_include_directories(st_oxr PRIVATE ${CMAKE_SOURCE_DIR}/src/external/openxr_includes)
			if(TARGET comp_gl)
				target_link_libraries(st_oxr PRIVATE comp_gl)
				target_compile_definitions(st_oxr PRIVATE XRT_HAVE_GL_NATIVE_COMPOSITOR)
			endif()
		endif()
	endif()

	# VK native compositor on macOS (direct Vulkan via MoltenVK)
	if(XRT_HAVE_VULKAN AND TARGET comp_vk_native)
		# oxr_session_gfx_vk_native.c already compiled from Metal block above
		target_link_libraries(st_oxr PRIVATE comp_vk_native)
		target_compile_definitions(st_oxr PRIVATE XRT_HAVE_VK_NATIVE_COMPOSITOR)
	endif()
endif()

# Qwerty driver: needed on all platforms for camera-centric view controls
# (oxr_session.c calls qwerty_get_view_state behind #ifdef XRT_BUILD_DRIVER_QWERTY)
if(XRT_BUILD_DRIVER_QWERTY AND TARGET drv_qwerty)
	target_link_libraries(st_oxr PRIVATE drv_qwerty_includes)
	target_compile_definitions(st_oxr PRIVATE XRT_BUILD_DRIVER_QWERTY)
endif()


# 3D geometry math now lives in aux_math (m_display3d_view, m_camera3d_view)

if(WIN32 AND XRT_HAVE_OPENGL)
	target_sources(st_oxr PRIVATE oxr_session_gfx_gl_win32.c)

	# GL native compositor support (bypasses Vulkan)
	if(TARGET comp_gl)
		target_sources(st_oxr PRIVATE oxr_session_gfx_gl_native.c)
		target_link_libraries(st_oxr PRIVATE comp_gl)
		target_compile_definitions(st_oxr PRIVATE XRT_HAVE_GL_NATIVE_COMPOSITOR)
	endif()
endif()

if(XRT_FEATURE_OPENXR_LAYER_FB_PASSTHROUGH)
	target_sources(st_oxr PRIVATE oxr_api_passthrough.c oxr_passthrough.c)
endif()

if(XRT_FEATURE_OPENXR_BODY_TRACKING_FB)
	target_sources(st_oxr PRIVATE oxr_api_body_tracking.c oxr_body_tracking.c)
endif()

if(XRT_FEATURE_OPENXR_FACIAL_TRACKING_HTC)
	target_sources(st_oxr PRIVATE oxr_api_face_tracking.c oxr_face_tracking.c)
endif()

if(XRT_FEATURE_OPENXR_FACE_TRACKING2_FB)
	target_sources(st_oxr PRIVATE oxr_api_face_tracking2_fb.c oxr_face_tracking2_fb.c)
endif()

target_link_libraries(
	st_oxr
	PRIVATE
		xrt-interfaces
		xrt-external-openxr
		aux_util
		aux_math
		aux_generated_bindings_helpers
		comp_client
		aux-includes
	PUBLIC aux_os
	)

if(XRT_FEATURE_RENDERDOC)
	target_link_libraries(st_oxr PUBLIC xrt-external-renderdoc)
	if(XRT_HAVE_LINUX)
		target_link_libraries(st_oxr PRIVATE ${CMAKE_DL_LIBS})
	endif()
endif()

target_include_directories(
	st_oxr
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}
		# multi/comp_multi_private.h is used for qwerty input device support
		${CMAKE_CURRENT_SOURCE_DIR}/../../compositor
		# Phase 2.C: oxr_workspace.c needs ipc_workspace_chrome_layout to
		# marshal the chrome layout struct to the IPC bridge.
		${CMAKE_CURRENT_SOURCE_DIR}/../../ipc
	INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..
	)
